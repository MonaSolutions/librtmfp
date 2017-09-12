/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "Base/Socket.h"
#include "Base/Runner.h"
#include "AMFWriter.h"
#include "Base/LostRate.h"
#include "RTMFP.h"
#include "Base/Congestion.h"

struct RTMFPSender : Base::Runner, virtual Base::Object {
	struct Packet : Base::Packet, virtual Base::Object {
		Packet(std::shared_ptr<Base::Buffer>& pBuffer, Base::UInt32 fragments, bool reliable) : fragments(fragments), Base::Packet(pBuffer), reliable(reliable), _sizeSent(0) {}
		void setSent() {
			if (_sizeSent)
				return;
			_sizeSent = size();
			if (!reliable)
				Base::Packet::reset(); // release immediatly an unreliable packet!
		}
		const bool   reliable;
		const Base::UInt32	fragments;
		Base::UInt32		sizeSent() const { return _sizeSent; }
	private:
		Base::UInt32		_sizeSent;
	};
	struct Session : virtual Base::Object {
		Session(Base::UInt32 farId, const std::shared_ptr<RTMFP::Engine>& pEncoder, const std::shared_ptr<Base::Socket>& pSocket, Base::Int64 time) :
			sendable(RTMFP::SENDABLE_MAX), socket(*pSocket), pEncoder(new RTMFP::Engine(*pEncoder)), farId(farId), initiatorTime(time),
			queueing(0), sendingSize(0), _pSocket(pSocket), sendLostRate(sendByteRate), sendTime(0), congested(false) {}

		bool isCongested() {
			Base::UInt64 queueSize(queueing);
			Base::UInt32 bufferSize(socket.sendBufferSize());
			// superior to buffer size to limit onFlush usage!
			queueSize = queueSize > bufferSize ? queueSize - bufferSize : 0;
			return queueSize && _congestion(queueSize, Base::Net::RTO_MAX);
		}

		Base::UInt32					farId;
		std::atomic<Base::Int64>		initiatorTime;
		std::shared_ptr<RTMFP::Engine>	pEncoder;
		Base::Socket&					socket;
		std::atomic<Base::Int64>		sendTime;
		Base::ByteRate					sendByteRate;
		Base::LostRate					sendLostRate;
		std::atomic<Base::UInt64>		queueing;
		std::atomic<Base::UInt64>		sendingSize;
		Base::UInt8						sendable;
		std::atomic<bool>				congested;
	private:
		std::shared_ptr<Base::Socket>	_pSocket; // to keep the socket open
		Base::Congestion				_congestion;
	};
	struct Queue : virtual Base::Object, std::deque<std::shared_ptr<Packet>> {
		template<typename SignatureType>
		Queue(Base::UInt64 id, Base::UInt64 flowId, const SignatureType& signature) : id(id), stage(0), stageSending(0), stageAck(0), signature(STR signature.data(), signature.size()), flowId(flowId) {}

		const Base::UInt64					id;
		const Base::UInt64					flowId;
		const std::string					signature;
		// used by RTMFPSender
		/// stageAck <= stageSending <= stage
		Base::UInt64						stage;
		Base::UInt64						stageSending;
		Base::UInt64						stageAck;
		std::deque<std::shared_ptr<Packet>>	sending;
	};

	// Flush usage!
	RTMFPSender(Base::UInt8 marker, const std::shared_ptr<Queue>& pQueue) : Base::Runner("RTMFPSender"), _marker(marker), pQueue(pQueue) {}

	Base::SocketAddress	address; // can change!
	std::shared_ptr<Session>	pSession;

protected:
	RTMFPSender(const char* name, Base::UInt8 marker, const std::shared_ptr<Queue>& pQueue) : Base::Runner(name), _marker(marker), pQueue(pQueue) {}
	RTMFPSender(const char* name, Base::UInt8 marker) : Runner(name), _marker(marker) {}

	std::shared_ptr<Queue>	pQueue;
	Base::UInt8				_marker;

private:
	bool		 run(Base::Exception& ex);
	virtual void run() {}
};

struct RTMFPCmdSender : RTMFPSender, virtual Base::Object {
	RTMFPCmdSender(Base::UInt8 cmd, Base::UInt8 marker) : RTMFPSender("RTMFPCmdSender", marker), _cmd(cmd) {}
private:
	void	run();

	Base::UInt8	_cmd;
};

struct RTMFPAcquiter : RTMFPSender, virtual Base::Object {
	RTMFPAcquiter(Base::UInt8 marker, const std::shared_ptr<RTMFPSender::Queue>& pQueue, Base::UInt64 stageAck) : RTMFPSender("RTMFPAcquiter", marker, pQueue), _stageAck(stageAck) {}
private:
	void	run();

	Base::UInt64	_stageAck;
};

struct RTMFPRepeater : RTMFPSender, virtual Base::Object {
	RTMFPRepeater(Base::UInt8 marker, const std::shared_ptr<RTMFPSender::Queue>& pQueue, Base::UInt8 fragments = 0) : RTMFPSender("RTMFPRepeater", marker, pQueue), _fragments(fragments) {}
private:
	void	run();
	void	sendAbandon(Base::UInt64 stage);

	Base::UInt8	_fragments;
};


struct RTMFPMessenger : RTMFPSender, virtual Base::Object {
	RTMFPMessenger(Base::UInt8 marker, const std::shared_ptr<RTMFPSender::Queue>& pQueue) : RTMFPSender("RTMFPMessenger", marker, pQueue), _flags(0), _fragments(0) {} // _flags must be initialized to 0!

	AMFWriter&	newMessage(bool reliable, const Base::Packet& packet) { _messages.emplace_back(reliable, packet); return _messages.back().writer; }

private:
	struct Message : private std::shared_ptr<Base::Buffer>, virtual Base::NullableObject {
		Message(bool reliable, const Base::Packet& packet) : reliable(reliable), packet(std::move(packet)), writer(*new Base::Buffer(RTMFP_MAX_PACKET_SIZE)) { reset(&writer->buffer()); }
		bool				reliable;
		AMFWriter			writer; // data
		const Base::Packet	packet; // footer
		explicit operator bool() const { return packet || writer ? true : false; }
	};
	Base::UInt32	headerSize();
	void			run();
	void			write(const Message& message);
	void			flush();

	std::deque<Message>	_messages;

	// current buffer =>
	std::shared_ptr<Base::Buffer>	_pBuffer;
	Base::UInt32					_fragments;
	Base::UInt8						_flags;
};
