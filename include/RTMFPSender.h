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

#include "Mona/Socket.h"
#include "Mona/Runner.h"
#include "AMFWriter.h"
#include "Mona/LostRate.h"
#include "RTMFP.h"

struct RTMFPSender : Mona::Runner, virtual Mona::Object {
	struct Packet : Mona::Packet, virtual Mona::Object {
		Packet(std::shared_ptr<Mona::Buffer>& pBuffer, Mona::UInt32 fragments, bool reliable) : fragments(fragments), Mona::Packet(pBuffer), reliable(reliable), _sizeSent(0) {}
		void setSent() {
			if (_sizeSent)
				return;
			_sizeSent = size();
			if (!reliable)
				Mona::Packet::reset(); // release immediatly an unreliable packet!
		}
		const bool   reliable;
		const Mona::UInt32	fragments;
		Mona::UInt32		sizeSent() const { return _sizeSent; }
	private:
		Mona::UInt32		_sizeSent;
	};
	struct Session : virtual Mona::Object {
		Session(Mona::UInt32 farId, const std::shared_ptr<RTMFP::Engine>& pEncoder, const std::shared_ptr<Mona::Socket>& pSocket) :
			sendable(RTMFP::SENDABLE_MAX), socket(*pSocket), pEncoder(new RTMFP::Engine(*pEncoder)), farId(farId),
			queueing(0), _pSocket(pSocket), sendLostRate(sendByteRate), sendTime(0), initiatorTime(0) {}
		Mona::UInt32					farId;
		std::atomic<Mona::Int64>		initiatorTime;
		std::shared_ptr<RTMFP::Engine>	pEncoder;
		Mona::Socket&					socket;
		std::atomic<Mona::Int64>		sendTime;
		Mona::ByteRate					sendByteRate;
		Mona::LostRate					sendLostRate;
		std::atomic<Mona::UInt64>		queueing;
		Mona::UInt8						sendable;
	private:
		std::shared_ptr<Mona::Socket>	_pSocket; // to keep the socket open
	};
	struct Queue : virtual Mona::Object, std::deque<std::shared_ptr<Packet>> {
		template<typename SignatureType>
		Queue(Mona::UInt64 id, Mona::UInt64 flowId, const SignatureType& signature) : id(id), stage(0), stageSending(0), stageAck(0), signature(STR signature.data(), signature.size()), flowId(flowId) {}

		const Mona::UInt64					id;
		const Mona::UInt64					flowId;
		const std::string					signature;
		// used by RTMFPSender
		/// stageAck <= stageSending <= stage
		Mona::UInt64						stage;
		Mona::UInt64						stageSending;
		Mona::UInt64						stageAck;
		std::deque<std::shared_ptr<Packet>>	sending;
	};

	// Flush usage!
	RTMFPSender(Mona::UInt8 marker, const std::shared_ptr<Queue>& pQueue) : Mona::Runner("RTMFPSender"), _marker(marker), pQueue(pQueue) {}

	Mona::SocketAddress	address; // can change!
	std::shared_ptr<Session>	pSession;

protected:
	RTMFPSender(const char* name, Mona::UInt8 marker, const std::shared_ptr<Queue>& pQueue) : Mona::Runner(name), _marker(marker), pQueue(pQueue) {}
	RTMFPSender(const char* name, Mona::UInt8 marker) : Runner(name), _marker(marker) {}

	std::shared_ptr<Queue>	pQueue;
	Mona::UInt8				_marker;

private:
	bool		 run(Mona::Exception& ex);
	virtual void run() {}
};

struct RTMFPCmdSender : RTMFPSender, virtual Mona::Object {
	RTMFPCmdSender(Mona::UInt8 cmd, Mona::UInt8 marker) : RTMFPSender("RTMFPCmdSender", marker), _cmd(cmd) {}
private:
	void	run();

	Mona::UInt8	_cmd;
};

struct RTMFPAcquiter : RTMFPSender, virtual Mona::Object {
	RTMFPAcquiter(Mona::UInt8 marker, const std::shared_ptr<RTMFPSender::Queue>& pQueue, Mona::UInt64 stageAck) : RTMFPSender("RTMFPAcquiter", marker, pQueue), _stageAck(stageAck) {}
private:
	void	run();

	Mona::UInt64	_stageAck;
};

struct RTMFPRepeater : RTMFPSender, virtual Mona::Object {
	RTMFPRepeater(Mona::UInt8 marker, const std::shared_ptr<RTMFPSender::Queue>& pQueue, Mona::UInt8 fragments = 0) : RTMFPSender("RTMFPRepeater", marker, pQueue), _fragments(fragments) {}
private:
	void	run();
	void	sendAbandon(Mona::UInt64 stage);

	Mona::UInt8	_fragments;
};


struct RTMFPMessenger : RTMFPSender, virtual Mona::Object {
	RTMFPMessenger(Mona::UInt8 marker, const std::shared_ptr<RTMFPSender::Queue>& pQueue) : RTMFPSender("RTMFPMessenger", marker, pQueue), _flags(0) {} // _flags must be initialized to 0!

	AMFWriter&	newMessage(bool reliable, const Mona::Packet& packet) { _messages.emplace_back(reliable, packet); return _messages.back().writer; }

private:
	struct Message : private std::shared_ptr<Mona::Buffer>, virtual Mona::NullableObject {
		Message(bool reliable, const Mona::Packet& packet) : reliable(reliable), packet(std::move(packet)), writer(*new Mona::Buffer()) { reset(&writer->buffer()); }
		bool				reliable;
		AMFWriter			writer; // data
		const Mona::Packet	packet; // footer
		explicit operator bool() const { return packet || writer ? true : false; }
	};
	Mona::UInt32	headerSize();
	void			run();
	void			write(const Message& message);
	void			flush();

	std::deque<Message>	_messages;

	// current buffer =>
	std::shared_ptr<Mona::Buffer>	_pBuffer;
	Mona::UInt32					_fragments;
	Mona::UInt8						_flags;
};
