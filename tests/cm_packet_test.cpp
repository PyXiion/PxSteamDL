// SPDX-License-Identifier: LGPL-3.0-or-later
#include "cm_packet.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

#include "proto.hpp"
#include "test_util.hpp"

namespace pxsteamdl::detail {
namespace {

using testing::Gzip;
using testing::ToBytes;

Packet SamplePacket() {
    Packet packet;
    packet.emsg = emsg::kServiceMethodCall;
    packet.steamid = 0x01A0000000000123ULL;
    packet.sessionid = -5;
    packet.jobid_source = 42;
    packet.target_job_name = "Service.Method#1";
    packet.body = ToBytes("body");
    return packet;
}

// The length-prefixed concatenation that a Multi carries.
Bytes MultiPayload(const Bytes& first, const Bytes& second) {
    Bytes payload;
    for (const Bytes* packet : {&first, &second}) {
        AppendLe32(payload, static_cast<std::uint32_t>(packet->size()));
        payload.insert(payload.end(), packet->begin(), packet->end());
    }
    return payload;
}

TEST(CmPacketTest, RoundTripsHeaderAndBody) {
    Packet parsed = ParsePacket(SerializePacket(SamplePacket()));
    EXPECT_EQ(parsed.emsg, emsg::kServiceMethodCall);
    EXPECT_EQ(parsed.steamid, 0x01A0000000000123ULL);
    EXPECT_EQ(parsed.sessionid, -5);
    EXPECT_EQ(parsed.jobid_source, 42u);
    EXPECT_EQ(parsed.jobid_target, kNoJob);
    EXPECT_EQ(parsed.target_job_name, "Service.Method#1");
    EXPECT_EQ(parsed.body, ToBytes("body"));
}

TEST(CmPacketTest, SetsProtobufFlagOnEMsg) {
    Bytes data = SerializePacket(SamplePacket());
    EXPECT_EQ(ReadLe32(data, 0), emsg::kServiceMethodCall | 0x80000000u);
}

TEST(CmPacketTest, RejectsTruncatedHeader) {
    Bytes data = SerializePacket(SamplePacket());
    EXPECT_THROW(ParsePacket(ByteSpan(data).first(6)), std::runtime_error);
    data.resize(10);  // header length still claims the full header
    EXPECT_THROW(ParsePacket(data), std::runtime_error);
}

TEST(CmPacketTest, ExpandsPlainMulti) {
    Packet first = SamplePacket();
    Packet second = SamplePacket();
    second.emsg = emsg::kClientLogonResponse;
    Packet multi;
    multi.emsg = emsg::kMulti;
    multi.body = EncodeBytes(2, MultiPayload(SerializePacket(first), SerializePacket(second)));

    std::vector<Packet> packets = ParseMessage(SerializePacket(multi));
    ASSERT_EQ(packets.size(), 2u);
    EXPECT_EQ(packets[0].emsg, emsg::kServiceMethodCall);
    EXPECT_EQ(packets[1].emsg, emsg::kClientLogonResponse);
}

TEST(CmPacketTest, ExpandsGzippedMulti) {
    Bytes payload = MultiPayload(SerializePacket(SamplePacket()), SerializePacket(SamplePacket()));
    Packet multi;
    multi.emsg = emsg::kMulti;
    multi.body = Concat({EncodeUint(1, payload.size()), EncodeBytes(2, Gzip(payload))});

    std::vector<Packet> packets = ParseMessage(SerializePacket(multi));
    ASSERT_EQ(packets.size(), 2u);
    EXPECT_EQ(packets[1].body, ToBytes("body"));
}

TEST(CmPacketTest, PassesThroughSinglePacket) {
    std::vector<Packet> packets = ParseMessage(SerializePacket(SamplePacket()));
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].target_job_name, "Service.Method#1");
}

}  // namespace
}  // namespace pxsteamdl::detail
