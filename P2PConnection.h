
#pragma once

#include "FlowManager.h"
#include "BandWriter.h"

/**************************************************
P2PConnection represents a direct P2P connection 
with another peer
*/
class P2PConnection : public FlowManager {
public:
	P2PConnection(FlowManager& parent, std::string id, Invoker* invoker, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) :
		peerId(id), _parent(parent), FlowManager(invoker, pOnStatusEvent, pOnMediaEvent) {

	}

	virtual ~P2PConnection() {
		close();
	}

	const std::string				peerId; // Peer Id of the peer connected

protected:

	// Close the connection properly
	virtual void close() {
		FlowManager::close();
	}

	/******* Internal functions for writers *******/
	virtual Mona::BinaryWriter&				writeMessage(Mona::UInt8 type, Mona::UInt16 length, RTMFPWriter* pWriter = NULL) { return _parent.writeMessage(type, length, pWriter); }

	void									flush() { return _parent.flush(); }

	// Return the size available in the current sender (or max size if there is no current sender)
	virtual Mona::UInt32					availableToWrite() { return _parent.availableToWrite(); }

private:
	FlowManager&	_parent; // RTMFPConnection related to
};
