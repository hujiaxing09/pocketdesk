#include "streamer.h"
#include "log.h"

#include <rtc/rtc.hpp>

#include <future>
#include <random>
#include <vector>

namespace pd {

namespace {
constexpr int kPayloadType = 96;
constexpr int kPlayoutDelayExtId = 1;
constexpr const char *kPlayoutDelayUri = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";
// Constrained Baseline：所有 Safari 都支持；以后再试 High
constexpr const char *kH264Fmtp =
    "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f";
constexpr auto kConnectTimeout = std::chrono::seconds(30);

std::string randomId() {
	static std::mt19937_64 rng{std::random_device{}()};
	static std::mutex m;
	std::lock_guard lock(m);
	return std::format("{:016x}", rng());
}

uint32_t randomSsrc() {
	static std::mt19937 rng{std::random_device{}()};
	return rng() | 1;
}
} // namespace

struct Streamer::Session {
	std::string id;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> track;
	std::shared_ptr<rtc::RtpPacketizationConfig> rtp;
	std::shared_ptr<rtc::DataChannel> control;
	std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
	std::atomic<bool> connected{false};
	std::atomic<bool> dead{false};
	bool waitingKeyframe = true; // 新连接从 IDR 开始发，只在截屏线程里读写
};

Streamer::Streamer(Options options) : mOptions(options) { rtc::InitLogger(rtc::LogLevel::Warning); }

Streamer::~Streamer() {
	std::map<std::string, std::shared_ptr<Session>> sessions;
	{
		std::lock_guard lock(mMutex);
		sessions.swap(mSessions);
	}
	for (auto &[id, s] : sessions)
		s->pc->close();
}

bool Streamer::createSession(std::string &id, std::string &offerSdp) {
	auto s = std::make_shared<Session>();
	s->id = randomId();

	rtc::Configuration cfg;
	cfg.portRangeBegin = mOptions.portBegin;
	cfg.portRangeEnd = mOptions.portEnd;
	cfg.disableAutoNegotiation = true;
	try {
		s->pc = std::make_shared<rtc::PeerConnection>(cfg);

		auto gathered = std::make_shared<std::promise<void>>();
		auto gatheredFuture = gathered->get_future();
		auto once = std::make_shared<std::once_flag>();
		s->pc->onGatheringStateChange([gathered, once](rtc::PeerConnection::GatheringState st) {
			if (st == rtc::PeerConnection::GatheringState::Complete)
				std::call_once(*once, [&] { gathered->set_value(); });
		});

		std::weak_ptr<Session> weak = s;
		s->pc->onStateChange([weak](rtc::PeerConnection::State st) {
			auto s = weak.lock();
			if (!s)
				return;
			using St = rtc::PeerConnection::State;
			if (st == St::Connected) {
				s->connected = true;
				log("[{}] 已连接", s->id);
			} else if (st == St::Disconnected || st == St::Failed || st == St::Closed) {
				if (!s->dead.exchange(true))
					log("[{}] 连接结束（{}）", s->id, int(st));
			}
		});

		const uint32_t ssrc = randomSsrc();
		rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
		media.addH264Codec(kPayloadType, kH264Fmtp);
		media.addSSRC(ssrc, "pocketdesk", "pocketdesk", "screen");
		media.addExtMap(rtc::Description::Entry::ExtMap(kPlayoutDelayExtId, kPlayoutDelayUri));
		s->track = s->pc->addTrack(media);

		s->rtp = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "pocketdesk", kPayloadType,
		                                                       rtc::H264RtpPacketizer::ClockRate);
		s->rtp->playoutDelayId = kPlayoutDelayExtId;
		s->rtp->playoutDelayMin = 0;
		s->rtp->playoutDelayMax = uint16_t(mOptions.playoutDelayMaxMs / 10);

		auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
		    rtc::NalUnit::Separator::StartSequence, s->rtp);
		packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(s->rtp));
		packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(2048));
		packetizer->addToChain(std::make_shared<rtc::PliHandler>([this] {
			mKeyRequest = true;
			++mPliCount;
		}));
		s->track->setMediaHandler(packetizer);
		s->track->onOpen([this, weak] {
			if (auto s = weak.lock())
				log("[{}] 视频轨已打开", s->id);
			mKeyRequest = true;
		});

		s->control = s->pc->createDataChannel("control");
		// 手机每 2 秒发回自己的统计（解码帧率、缓冲、丢包），写进日志方便分析
		s->control->onMessage([weak](rtc::message_variant msg) {
			auto s = weak.lock();
			if (!s)
				return;
			if (auto text = std::get_if<std::string>(&msg))
				log("[{}] 手机统计 {}", s->id, *text);
		});
		s->pc->setLocalDescription(rtc::Description::Type::Offer);

		if (gatheredFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
			log("[{}] ICE 候选收集超时，用已有的候选继续", s->id);

		auto desc = s->pc->localDescription();
		if (!desc) {
			log("[{}] 没有生成 offer", s->id);
			return false;
		}
		offerSdp = std::string(*desc);
	} catch (const std::exception &e) {
		log("创建 WebRTC 连接失败: {}", e.what());
		return false;
	}

	id = s->id;
	{
		std::lock_guard lock(mMutex);
		mSessions[s->id] = s;
	}
	log("[{}] 已发出 offer", s->id);
	return true;
}

bool Streamer::acceptAnswer(const std::string &id, const std::string &answerSdp) {
	std::shared_ptr<Session> s;
	{
		std::lock_guard lock(mMutex);
		auto it = mSessions.find(id);
		if (it != mSessions.end())
			s = it->second;
	}
	if (!s) {
		log("[{}] 收到 answer，但连接不存在", id);
		return false;
	}
	try {
		s->pc->setRemoteDescription(rtc::Description(answerSdp, rtc::Description::Type::Answer));
	} catch (const std::exception &e) {
		log("[{}] answer 无效: {}", id, e.what());
		return false;
	}
	bool playout = answerSdp.find(kPlayoutDelayUri) != std::string::npos;
	log("[{}] 已收到 answer，手机{}支持 playout-delay", id, playout ? "" : "不");
	return true;
}

bool Streamer::hasViewers() {
	std::lock_guard lock(mMutex);
	for (auto &[id, s] : mSessions)
		if (!s->dead && s->track && s->track->isOpen())
			return true;
	return false;
}

void Streamer::sendVideo(const uint8_t *data, size_t size, bool keyframe) {
	std::lock_guard lock(mMutex);
	auto now = std::chrono::steady_clock::now();
	for (auto &[id, s] : mSessions) {
		if (s->dead || !s->track->isOpen())
			continue;
		if (s->waitingKeyframe) {
			if (!keyframe) {
				mKeyRequest = true;
				continue;
			}
			s->waitingKeyframe = false;
		}
		std::chrono::duration<double> elapsed = now - s->created;
		try {
			s->track->sendFrame(reinterpret_cast<const std::byte *>(data), size,
			                    rtc::FrameInfo(elapsed));
		} catch (const std::exception &e) {
			log("[{}] 发送视频失败: {}", id, e.what());
		}
	}
}

void Streamer::sendControl(const std::string &json) {
	std::lock_guard lock(mMutex);
	for (auto &[id, s] : mSessions) {
		if (s->dead || !s->control || !s->control->isOpen())
			continue;
		try {
			s->control->send(json);
		} catch (const std::exception &) {
		}
	}
}

void Streamer::sweep() {
	std::vector<std::shared_ptr<Session>> removed;
	{
		std::lock_guard lock(mMutex);
		auto now = std::chrono::steady_clock::now();
		for (auto it = mSessions.begin(); it != mSessions.end();) {
			auto &s = it->second;
			if (!s->connected && now - s->created > kConnectTimeout && !s->dead) {
				log("[{}] 30 秒内没有连上，放弃", s->id);
				s->dead = true;
			}
			if (s->dead) {
				removed.push_back(s);
				it = mSessions.erase(it);
			} else {
				++it;
			}
		}
	}
	// 在锁外关闭，避免和 libdatachannel 的回调线程互相等待
	for (auto &s : removed)
		s->pc->close();
}

} // namespace pd
