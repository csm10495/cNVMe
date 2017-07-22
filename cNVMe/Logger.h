/*
This file is part of cNVMe and is released under the MIT License
(C) - Charles Machalow - 2017
Logger.h - A header file for the Logging
*/

#pragma once

#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// Macros to make this easier to work with
#define LOG_ERROR(txt) cnvme::logging::theLogger.log(std::string(__func__) + "():" \
+ std::to_string(__LINE__) + " - [" + cnvme::logging::Logger::loggingLevelToString(cnvme::logging::ERROR) + "] - " + txt, cnvme::logging::ERROR);
#define LOG_INFO(txt) cnvme::logging::theLogger.log(std::string(__func__) + "():" \
+ std::to_string(__LINE__) + " - [" + cnvme::logging::Logger::loggingLevelToString(cnvme::logging::INFO) + "] - " + txt, cnvme::logging::INFO);
#define LOG_SET_LEVEL(level) cnvme::logging::theLogger.setLevel((cnvme::logging::LOGGING_LEVEL)level);
#define ASSERT(txt) cnvme::logging::theLogger._assert(std::string(__func__), txt);
#define ASSERT_IF(cond, txt) cnvme::logging::theLogger._assert_if(std::string(__func__), cond, txt);
// The following two have braces at the end to make sure they get used together. Use to hide logging on a thread.
#define _HIDE_LOG_THREAD() cnvme::logging::theLogger.addHiddenThread(std::this_thread::get_id()); {
#define _UNHIDE_LOG_THREAD() cnvme::logging::theLogger.removeHiddenThread(std::this_thread::get_id()); }
// The following two have braces at the end to make sure they get used together. Used to not print asserts on a thread
#define _START_ASSERT_QUIET() cnvme::logging::theLogger.setAssertQuiet(true, std::this_thread::get_id()); {
#define _END_ASSERT_QUIET() cnvme::logging::theLogger.setAssertQuiet(false, std::this_thread::get_id()); };

#define CLEARED_STATUS "Healthy"

namespace cnvme
{
	namespace logging
	{
		/// <summary>
		/// Used to know the level of logging
		/// </summary>
		enum LOGGING_LEVEL
		{
			SILENT,
			ERROR,
			INFO,
		};

		class Logger
		{
		public:
			/// <summary>
			/// Constructor
			/// </summary>
			Logger();

			/// <summary>
			/// Function to set the logging level
			/// </summary>
			/// <param name="level">Logging level</param>
			void setLevel(LOGGING_LEVEL level);

			/// <summary>
			/// Gets the current logging level
			/// </summary>
			/// <returns>The logging level</returns>
			LOGGING_LEVEL getLevel() const;

			/// <summary>
			/// Logs the given text if the level is higher than required for the stream
			/// </summary>
			/// <param name="txt"></param>
			void log(std::string txt, LOGGING_LEVEL level);

			/// <summary>
			/// Helper to get a string from the logging level
			/// </summary>
			/// <param name="level">level to convert to string</param>
			/// <returns>String of the level</returns>
			static std::string loggingLevelToString(LOGGING_LEVEL level)
			{
				if (level == ERROR)
				{
					return "Error";
				}
				else if (level == INFO)
				{
					return "Info";
				}
				return "Unknown";
			}

			/// <summary>
			/// Set the simulation's status
			/// </summary>
			/// <param name="status">A message to say what's up.</param>
			void setStatus(std::string status);

			/// <summary>
			/// Return the current status
			/// </summary>
			/// <returns>string</returns>
			std::string getStatus();

			/// <summary>
			/// Clears the current status
			/// </summary>
			void clearStatus();

			/// <summary>
			/// Don't set this... normally. Will silence asserts in terms of printing to stderr
			/// </summary>
			/// <param name="assertQuiet">True if asserts should be quiet, False otherwise</param>
			/// <param name="threadId">current thread id</param>
			void setAssertQuiet(bool assertQuiet, const std::thread::id& threadId);

			/// <summary>
			/// Add a thread to hide printing for
			/// Don't do this. Use FAIL_IF_HIDE_LOG in tests.cpp only.
			/// </summary>
			/// <param name="hiddenThread">Thread to hide printing for</param>
			void addHiddenThread(const std::thread::id& hiddenThread);

			/// <summary>
			/// Remove a thread from hiding printing for
			/// /// Don't do this. Use FAIL_IF_HIDE_LOG in tests.cpp only.
			/// </summary>
			/// <param name="hiddenThread">Thread to remove hidden printing for</param>
			void removeHiddenThread(const std::thread::id& hiddenThread);

			/// <summary>
			/// Cause an assert with the given txt
			/// Use the ASSERT macro. Do not call directly
			/// </summary>
			void _assert(std::string funcName, std::string txt);

			/// <summary>
			/// Asserts if a condition is met
			/// Use the ASSERT_IF macro. do not call directly
			/// </summary>
			void _assert_if(std::string funcName, bool condition, std::string txt);

		private:
			/// <summary>
			/// Current level of logging
			/// </summary>
			LOGGING_LEVEL Level;

			/// <summary>
			/// Gets the current time as a string
			/// </summary>
			std::string getCurrentTime();

			/// <summary>
			/// Mutex for printing
			/// </summary>
			std::mutex Mutex;

			/// <summary>
			/// Current status
			/// </summary>
			std::string Status;

			/// <summary>
			/// True if asserts print to stderr
			/// </summary>
			bool AssertLoud;

			/// <summary>
			/// Set of threads we are currently not printing for.
			/// </summary>
			std::set<std::thread::id> HiddenThreads;

			/// <summary>
			/// Set of threads that are currently not asserting
			/// </summary>
			std::set<std::thread::id> AssertQuietThreads;
		};

		/// <summary>
		/// Global logger, used for logging
		/// </summary>
		extern Logger theLogger;
	}
}


