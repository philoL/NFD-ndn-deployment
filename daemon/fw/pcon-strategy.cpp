/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2016 Klaus Schneider, The University of Arizona
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Klaus Schneider <klaus@cs.arizona.edu>
 */

#include "pcon-strategy.hpp"

#include "str-helper.hpp"
#include <ns3/node-list.h>
#include "forwarder.hpp"
#include "algorithm.hpp"
#include "../face/face.hpp"


namespace nfd {
namespace fw {

NFD_REGISTER_STRATEGY(PconStrategy);

// Used for logging:
shared_ptr<std::ofstream> PconStrategy::m_os;
boost::mutex PconStrategy::m_mutex;

PconStrategy::PconStrategy(Forwarder& forwarder, const Name& name)
    : Strategy(forwarder),
        m_ownForwarder(forwarder),
        m_lastFWRatioUpdate(time::steady_clock::TimePoint::min()),
        m_lastFWWrite(time::steady_clock::TimePoint::min()),
        TIME_BETWEEN_FW_UPDATE(time::milliseconds(110)),
        // 20ms between each writing of the forwarding table
        TIME_BETWEEN_FW_WRITE(time::milliseconds(20)),
        m_nonce(0)
{
  // Write header of forwarding percentage table
  if (m_os == nullptr) {
    m_os = make_shared<std::ofstream>();
    m_os->open("results/fwperc.txt");
    *m_os << "Time" << "\tNode" << "\tPrefix" << "\tFaceId" << "\ttype" << "\tvalue"
        << "\n";
  }

  // Start all FIB entries by sending on the shortest path?
  // If false: start with an equal split.
  INIT_SHORTEST_PATH = StrHelper::getEnvVariable("INIT_SHORTEST_PATH", true);

  // How much the forwarding percentage changes for each received congestion mark
  CHANGE_PER_MARK = StrHelper::getDoubleEnvVariable("CHANGE_PER_MARK", 0.02);

  // How much of the traffic should be used for probing?
  PROBING_PERCENTAGE = StrHelper::getDoubleEnvVariable("PROBING_PERCENTAGE", 0.001);
}

PconStrategy::~PconStrategy()
{
  m_os->close();
}


const Name&
PconStrategy::getStrategyName()
{
  static Name strategyName("/localhost/nfd/strategy/pcon/%FD%03");
  return strategyName;
}


MtForwardingInfo&
PconStrategy::getMeasurementsEntryInfo(const fib::Entry& entry)
{
  measurements::Entry* measurementsEntry = this->getMeasurements().get(entry);
  return this->getMeasurementsEntryInfo(measurementsEntry);
}

//PconStrategy::MeasurementsEntryInfo&
//PconStrategy::getMeasurementsEntryInfo(const shared_ptr<pit::Entry>& entry)
//{
//  measurements::Entry* measurementsEntry = this->getMeasurements().get(*entry);
//  return this->getMeasurementsEntryInfo(measurementsEntry);
//}

MtForwardingInfo&
PconStrategy::getMeasurementsEntryInfo(measurements::Entry* entry)
{
  BOOST_ASSERT(entry != nullptr);
  MtForwardingInfo* info = nullptr;
  bool isNew = false;
  std::tie(info, isNew) = entry->insertStrategyInfo<MtForwardingInfo>();
  if (!isNew) {
    return *info;
  }

  measurements::Entry* parentEntry = this->getMeasurements().getParent(*entry);
  if (parentEntry != nullptr) {
    MtForwardingInfo& parentInfo = this->getMeasurementsEntryInfo(parentEntry);
    info->inheritFrom(parentInfo);
  }

  assert(info != nullptr);
  return *info;
}

void
PconStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
    const shared_ptr<pit::Entry>& pitEntry)
{
  // Retrieving measurement info
  const fib::Entry& fibEntry = lookupFib(*pitEntry);

  MtForwardingInfo& measurementInfo = getMeasurementsEntryInfo(fibEntry);
//  assert(measurementInfo != nullptr);

  // Prefix info not found, create new prefix measurements
//  if (measurementInfo == nullptr) {
//    measurementInfo =
////        StrHelper::addPrefixMeasurements(fibEntry,
////        this->getMeasurements());
//    const Name prefixName = fibEntry.getPrefix();
//    measurementInfo->setPrefix(prefixName.toUri());

  initializeForwMap(measurementInfo, fibEntry.getNextHops());
  // TODO: Initialize?
//  }

  bool wantNewNonce = false;
  // Block similar interests that are not from the same host.

  // PIT entry for incoming Interest already exists.
  //  if (pitEntry->hasUnexpiredOutRecords()) {

  // TODO: Problem begin?
  if (hasPendingOutRecords(*pitEntry)) {
    //    pitEntry->has

    // Check if request comes from a new incoming face:
    bool requestFromNewFace = false;

    // TODO: Problem here!
    for (const auto& in : pitEntry->getInRecords()) {
      time::steady_clock::Duration lastRenewed = in.getLastRenewed()
          - time::steady_clock::now();
      if (lastRenewed.count() >= 0) {
        requestFromNewFace = true;
      }
    }

    // Received retx from same face. Forward with new nonce.
    if (!requestFromNewFace) {
      wantNewNonce = true;
    }
    // Request from new face. Suppress.
    else {
//      std::cout << StrHelper::getTime() << " Node " << m_ownForwarder.getNodeId()
//          << " suppressing request from new face: " << inFace.getId() << "!\n";
      return;
    }
  }
  // TODO: Problem end?

  fib::NextHop outNH = fibEntry.getNextHops().front();

  // Random number between 0 and 1.
  double r = ((double) rand() / (RAND_MAX));

  double percSum = 0;
  // Add all eligbile faces to list (excludes current downstream)
  fib::NextHopList eligbleNHs;
  for (auto n : fibEntry.getNextHops()) {
    if (StrHelper::predicate_NextHop_eligible(pitEntry, n, inFace.getId())) {
      eligbleNHs.emplace_back(n);
//      eligbleFaces.emplace_back(make_shared<Face>(n.getFace()));
      // Add up percentage Sum.
      percSum += measurementInfo.getforwPerc(n.getFace().getId());
    }
  }

  if (eligbleNHs.size() < 1) {
    std::cout << "Blocked interest from face: " << inFace.getId()
        << " (no eligible faces)\n";
    return;
  }

  // If only one face: Send out on it.
  else if (eligbleNHs.size() == 1) {
    outNH = eligbleNHs.front();
  }

  // More than 1 eligible face!
  else {
    // If percSum == 0, there is likely a problem in the routing configuration,
    // e.g. only the downstream has a forwPerc > 0.
    assert(percSum > 0);

    // Write fw percentage to file
    if (time::steady_clock::now() >= m_lastFWWrite + TIME_BETWEEN_FW_WRITE) {
      m_lastFWWrite = time::steady_clock::now();
//      writeFwPercMap(m_ownForwarder, measurementInfo);
    }

    // Choose face according to current forwarding percentage:
    double forwPerc = 0;
    for (auto nh : eligbleNHs) {
      forwPerc += measurementInfo.getforwPerc(nh.getFace().getId()) / percSum;
      assert(forwPerc >= 0);
      assert(forwPerc <= 1.1);
      if (r < forwPerc) {
        outNH = nh;
        break;
      }
    }
  }

  // Error: No possible outgoing face.
  assert(!eligbleNHs.empty());
//  if (outNH == nullptr) {
//    std::cout << StrHelper::getTime() << " node " << m_ownForwarder.getNodeId()
//        << ", inface " << inFace.getLocalUri() << inFace.getId() << ", interest: "
//        << interest.getName() << " outface nullptr\n\n";
//  }
//  assert(outFace != nullptr);

  // If outgoing face is congested: mark PIT entry as congested.
//  if (StrHelper::isCongested(m_ownForwarder, outNH.getFace().getId())) {
//    pitEntry->m_congMark = true;
//    // TODO: Reduce forwarding percentage on outgoing face?
//    std::cout << StrHelper::getTime() << " node " << m_ownForwarder.getNodeId()
//        << " marking PIT Entry " << pitEntry->getName() << ", inface: " << inFace.getId()
//        << ", outface: " << outFace->getId() << "\n";
//  }

  this->sendInterest(pitEntry, outNH.getFace(), interest);

//  this->sendInterest(pitEntry, outNH.getFace(), wantNewNonce);

  // Probe other faces
  if (r <= PROBING_PERCENTAGE) {
    probeInterests(outNH.getFace().getId(), fibEntry.getNextHops(), pitEntry);
  }
}

/**
 * Probe all faces other than the current outgoing face (which is used already).
 */
void
PconStrategy::probeInterests(const FaceId outFaceId, const fib::NextHopList& nexthops,
    shared_ptr<pit::Entry> pitEntry)
{
  for (auto n : nexthops) {
    if (n.getFace().getId() != outFaceId) {
      Interest interest{};
      interest.setNonce(m_nonce);
      m_nonce++;
      this->sendInterest(pitEntry, n.getFace(), interest);
//      this->sendInterest(pitEntry, n.getFace(), true);
    }
  }
}

void
PconStrategy::beforeSatisfyInterest(const shared_ptr<pit::Entry>& pitEntry,
    const Face& inFace, const Data& data)
{
  const fib::Entry& fibEntry = lookupFib(*pitEntry);

  Name currentPrefix;
  // TODO:
  MtForwardingInfo& measurementInfo = getMeasurementsEntryInfo(fibEntry);

//  std::tie(currentPrefix, measurementInfo) = StrHelper::findPrefixMeasurementsLPM(
//      *pitEntry, this->getMeasurements());
//  if (measurementInfo == nullptr) {
//    std::cout << StrHelper::getTime() << "Didn't find measurement entry: "
//        << pitEntry->getName() << "\n";
//  }
//  assert(measurementInfo != nullptr);

  int congMark = data.getCongestionMark();

//  int8_t congMark = StrHelper::getCongMark(data);
//  int8_t nackType = StrHelper::getNackType(data);

  // If there is more than 1 face and data doesn't come from local content store
  // Then update forwarding ratio

  // TODO: Removed:
  // && !inFace.isLocal()
  if (measurementInfo.getFaceCount() > 1 && !(inFace.getScope() == ndn::nfd::FACE_SCOPE_LOCAL)
      && inFace.getLocalUri().toString() != "contentstore://") {

    bool updateBasedOnNACK = false;
    // For marked NACKs: Only adapt fw percentage each Update Interval (default: 110ms)
//    if (nackType == NACK_TYPE_MARK
//        && (time::steady_clock::now() >= m_lastFWRatioUpdate + TIME_BETWEEN_FW_UPDATE)) {
//      m_lastFWRatioUpdate = time::steady_clock::now();
//      updateBasedOnNACK = true;
//    }

    // If Data congestion marked or NACK updates ratio:
    if (congMark || updateBasedOnNACK) {

      double fwPerc = measurementInfo.getforwPerc(inFace.getId());
      double change_perc = CHANGE_PER_MARK * fwPerc;

      StrHelper::reduceFwPerc(measurementInfo, inFace.getId(), change_perc);
//      writeFwPercMap(m_ownForwarder, measurementInfo);
    }
  }

//  bool pitMarkedCongested = false;
//  if (pitEntry.m_congMark == true) {
//    std::cout << StrHelper::getTime() << " node " << m_ownForwarder.getNodeId()
//        << " found marked PIT Entry: " << pitEntry->getName() << ", face: "
//        << inFace.getId() << "!\n";
//    pitMarkedCongested = true;
//  }
//  for (auto n : pitEntry->getInRecords()) {
//    bool downStreamCongested = StrHelper::isCongested(m_ownForwarder,
//        n.getFace()->getId());
//
//    int8_t markSentPacket = std::max(
//        std::max((int8_t) congMark, (int8_t) downStreamCongested),
//        (int8_t) pitMarkedCongested);
//
//    ndn::CongestionTag tag3 = ndn::CongestionTag(nackType, markSentPacket, false, false);
//    //    tag3.setCongMark(markSentPacket);
//    assert(tag3.getCongMark() == markSentPacket);
//
//    data.setTag(make_shared<ndn::CongestionTag>(tag3));
//  }

}

/**
 * On expiring Interest: Reduce forwarding percentage of outgoing faces.
 *
 * Note that in normal operations there shouldn't be many expired Interests.
 */
//void
//PconStrategy::beforeExpirePendingInterest(shared_ptr<pit::Entry> pitEntry)
//{
//  Name currentPrefix;
//  shared_ptr<PconStrategy::MeasurementsEntryInfo> measurementInfo;
//  std::tie(currentPrefix, measurementInfo) = StrHelper::findPrefixMeasurementsLPM(
//      *pitEntry, this->getMeasurements());
//  assert(measurementInfo != nullptr);
//
//  // Reduce forwarding percentage of all expired OutRecords:
//  for (auto o : pitEntry->getOutRecords()) {
//
//    if (measurementInfo->getFaceCount() > 1
//        && measurementInfo->getforwPerc(
//            pitEntry->getOutRecords().front().getFace()->getId()) > 0.0) {
//
//      std::cout << StrHelper::getTime() << " PIT Timeout : " << pitEntry->getName()
//          << ", " << o.getFace()->getLocalUri() << o.getFace()->getId() << "! \n";
//
//      StrHelper::reduceFwPerc(measurementInfo,
//          pitEntry->getOutRecords().front().getFace()->getId(), CHANGE_PER_MARK);
//      writeFwPercMap(m_ownForwarder, measurementInfo);
//    }
//  }
//}


void
PconStrategy::initializeForwMap(MtForwardingInfo& measurementInfo,
    std::vector<fib::NextHop> nexthops)
{
  int lowestId = std::numeric_limits<int>::max();

  int localFaceCount = 0;
  for (fib::NextHop n : nexthops) {
    if (n.getFace().getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {
      localFaceCount++;
    }
    if (n.getFace().getId() < lowestId) {
      lowestId = n.getFace().getId();
    }
  }

  ns3::Ptr<ns3::Node> node = ns3::NodeList::GetNode(ns3::Simulator::GetContext()); // Get Current node from simulator context

  std::cout << StrHelper::getTime() << " Init FW node " << node->GetId() << ": ";
  for (auto n : nexthops) {
    double perc = 0.0;
    if (localFaceCount > 0 || INIT_SHORTEST_PATH) {
      if (n.getFace().getId() == lowestId) {
        perc = 1.0;
      }
    }
    // Split forwarding percentage equally.
    else {
      perc = 1.0 / (double) nexthops.size();
    }
    std::cout << "face " << n.getFace().getLocalUri() << n.getFace().getId();
    std::cout << "=" << perc << ", ";
    measurementInfo.setforwPerc(n.getFace().getId(), perc);
  }
  std::cout << "\n";

//  writeFwPercMap(m_ownForwarder, measurementInfo);
}

void
PconStrategy::writeFwPercMap(Forwarder& ownForwarder,
    MtForwardingInfo* measurementInfo)
{
  boost::mutex::scoped_lock scoped_lock(m_mutex);
//  std::string prefix = measurementInfo->getPrefix();
  ns3::Ptr<ns3::Node> node = ns3::NodeList::GetNode(ns3::Simulator::GetContext()); // Get Current node from simulator context

  for (auto f : measurementInfo->getForwPercMap()) {
    StrHelper::printFwPerc(m_os, node->GetId(), "prefix", f.first, "forwperc",
        f.second);
  }
  m_os->flush();
}

} // namespace fw
} // namespace nfd