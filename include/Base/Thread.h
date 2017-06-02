/*
This file is a part of MonaSolutions Copyright 2017
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This program is free software: you can redistribute it and/or
modify it under the terms of the the Mozilla Public License v2.0.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
Mozilla Public License v. 2.0 received along this program for more
details (or else see http://mozilla.org/MPL/2.0/).

*/

#pragma once
#include "Base/Mona.h"
#include "Base/Exceptions.h"
#include "Base/Signal.h"
#include <thread>

namespace Base {

struct Thread : virtual Object {
	enum Priority {
		PRIORITY_LOWEST=0,
		PRIORITY_LOW,
		PRIORITY_NORMAL,
		PRIORITY_HIGH,
		PRIORITY_HIGHEST
	};

	bool						start(Exception& ex, Priority priority = PRIORITY_NORMAL);
	virtual void				stop();

	const char*					name() const { return _name; }
	bool						running() const { return !_stop; }
	
	static unsigned				ProcessorCount() { unsigned result(std::thread::hardware_concurrency());  return result > 0 ? result : 1; }
	template<typename DurationType>
	static void					Sleep(DurationType duration) { std::this_thread::sleep_for(std::chrono::milliseconds(duration)); }

	//static const std::string&	CurrentName() { return _Name; }
	static UInt32				CurrentId();
	static const UInt32			MainId;

	virtual ~Thread();

	struct ChangeName : virtual Object {
		//template <typename ...Args>
		ChangeName(const char* name) : _name(name) { SetSystemName(name); /*_oldName.swap(_Name);*/ }
		~ChangeName() { /*SetSystemName(_Name = std::move(_oldName));*/ }
		operator const std::string&() const { return _name; }
	private:
		std::string _name;
		//std::string _oldName;
	};
protected:
	Thread(const char* name);

	Signal wakeUp;
	//template <typename ...Args>
	void setName(const char* name) { SetSystemName(name); }
private:
	virtual bool run(Exception& ex, const volatile bool& stopping) = 0;
	void		 process();

	static void  SetSystemName(const char* name);
	
	//static thread_local std::string		_Name;
	static thread_local Thread*			_Me;

	const char*		_name;
	Priority		_priority;
	bool			_stop;
	volatile bool	_stopping;

	std::mutex		_mutex; // protect _thread
	std::thread		_thread;
};


} // namespace Base
