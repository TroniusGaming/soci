//
// Copyright (C) 2004-2006 Maciej Sobczak, Stephen Hutton, Rafal Bobrowski
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#define SOCI_FIREBIRD_SOURCE
#include "soci/firebird/soci-firebird.h"
#include "firebird/error-firebird.h"
#include "soci/session.h"
#include <locale>
#include <map>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#define Sleep(time) Sleep(time)
#else
#include <unistd.h>
#define Sleep(time) usleep(time*1000)
#endif

using namespace soci;
using namespace soci::details::firebird;

namespace
{

// Helpers of explodeISCConnectString() for reading words from a string. "Word"
// here is defined very loosely as just a sequence of non-space characters.
//
// All these helper functions update the input iterator to point to the first
// character not consumed by them.

// Advance the input iterator until the first non-space character or end of the
// string.
void skipWhiteSpace(std::string::const_iterator& i, std::string::const_iterator const &end)
{
    std::locale const loc;
    for (; i != end; ++i)
    {
        if (!std::isspace(*i, loc))
            break;
    }
}

// Return the string of all characters until the first space or the specified
// delimiter.
//
// Throws if the first non-space character after the end of the word is not the
// delimiter. However just returns en empty string, without throwing, if
// nothing is left at all in the string except for white space.
std::string
getWordUntil(std::string const &s, std::string::const_iterator &i, char delim)
{
    std::string::const_iterator const end = s.end();
    skipWhiteSpace(i, end);

    // We need to handle this case specially because it's not an error if
    // nothing at all remains in the string. But if anything does remain, then
    // we must have the delimiter.
    if (i == end)
        return std::string();

    // Simply put anything until the delimiter into the word, stopping at the
    // first white space character.
    std::string word;
    std::locale const loc;
    for (; i != end; ++i)
    {
        if (*i == delim)
            break;

        if (std::isspace(*i, loc))
        {
            skipWhiteSpace(i, end);
            if (i == end || *i != delim)
            {
                std::ostringstream os;
                os << "Expected '" << delim << "' at position "
                   << (i - s.begin() + 1)
                   << " in Firebird connection string \""
                   << s << "\".";

                throw soci_error(os.str());
            }

            break;
        }

        word += *i;
    }

    if (i == end)
    {
        std::ostringstream os;
        os << "Expected '" << delim
           << "' not found before the end of the string "
           << "in Firebird connection string \""
           << s << "\".";

        throw soci_error(os.str());
    }

    ++i;    // Skip the delimiter itself.

    return word;
}

// Return a possibly quoted word, i.e. either just a sequence of non-space
// characters or everything inside a double-quoted string.
//
// Throws if the word is quoted and the closing quote is not found. However
// doesn't throw, just returns an empty string if there is nothing left.
std::string
getPossiblyQuotedWord(std::string const &s, std::string::const_iterator &i)
{
    std::string::const_iterator const end = s.end();
    skipWhiteSpace(i, end);

    std::string word;

    if (i != end && *i == '"')
    {
        for (;;)
        {
            if (++i == end)
            {
                std::ostringstream os;
                os << "Expected '\"' not found before the end of the string "
                      "in Firebird connection string \""
                   << s << "\".";

                throw soci_error(os.str());
            }

            if (*i == '"')
            {
                ++i;
                break;
            }

            word += *i;
        }
    }
    else // Not quoted.
    {
        std::locale const loc;
        for (; i != end; ++i)
        {
            if (std::isspace(*i, loc))
                break;

            word += *i;
        }
    }

    return word;
}

// retrieves parameters from the uniform connect string which is supposed to be
// in the form "key=value[ key2=value2 ...]" and the values may be quoted to
// allow including spaces into them. Notice that currently there is no way to
// include both a space and a double quote in a value.
std::map<std::string, std::string>
explodeISCConnectString(std::string const &connectString)
{
    std::map<std::string, std::string> parameters;

    std::string key, value;
    for (std::string::const_iterator i = connectString.begin(); ; )
    {
        key = getWordUntil(connectString, i, '=');
        if (key.empty())
            break;

        value = getPossiblyQuotedWord(connectString, i);

        parameters.insert(std::pair<std::string, std::string>(key, value));
    }

    return parameters;
}

// extracts given parameter from map previusly build with explodeISCConnectString
bool getISCConnectParameter(std::map<std::string, std::string> const & m, std::string const & key,
    std::string & value)
{
    std::map <std::string, std::string> :: const_iterator i;
    value.clear();

    i = m.find(key);

    if (i != m.end())
    {
        value = i->second;
        return true;
    }
    else
    {
        return false;
    }
}

void setDPBOption(std::string& dpb, int const option, std::string const & value)
{

    if (dpb.empty())
    {
        dpb.append(1, static_cast<char>(isc_dpb_version1));
    }

    // now we are adding new option
    dpb.append(1, static_cast<char>(option));
    dpb.append(1, static_cast<char>(value.size()));
    dpb.append(value);
}

} // namespace anonymous

firebird_session_backend::firebird_session_backend(
    connection_parameters const & parameters) : dbhp_(0), trhp_(0)
                                         , decimals_as_strings_(false)
{
    // extract connection parameters
    std::map<std::string, std::string>
        params(explodeISCConnectString(parameters.get_connect_string()));

    ISC_STATUS stat[stat_size];
    std::string param;

    // preparing connection options
    std::string dpb;
    if (getISCConnectParameter(params, "user", param))
    {
        setDPBOption(dpb, isc_dpb_user_name, param);
    }

    if (getISCConnectParameter(params, "password", param))
    {
        setDPBOption(dpb, isc_dpb_password, param);
    }

    if (getISCConnectParameter(params, "role", param))
    {
        setDPBOption(dpb, isc_dpb_sql_role_name, param);
    }

    if (getISCConnectParameter(params, "charset", param))
    {
        setDPBOption(dpb, isc_dpb_lc_ctype, param);
    }

    if (getISCConnectParameter(params, "service", param) == false)
    {
        throw soci_error("Service name not specified.");
    }

    // connecting data base
    if (isc_attach_database(stat, static_cast<short>(param.size()),
        const_cast<char*>(param.c_str()), &dbhp_,
        static_cast<short>(dpb.size()), const_cast<char*>(dpb.c_str())))
    {
        throw_iscerror(stat);
    }

    if (getISCConnectParameter(params, "decimals_as_strings", param))
    {
        decimals_as_strings_ = param == "1" || param == "Y" || param == "y";
    }
}

void firebird_session_backend::convert_tr_flags(std::vector<ISC_SCHAR> & flags) SOCI_NOEXCEPT
{
    if (!flags.empty() && flags[0] != isc_tpb_version3)
        flags.insert(flags.begin(), (ISC_SCHAR)isc_tpb_version3);

    if (trflags_ & details::trf_read)
        flags.push_back((ISC_SCHAR)isc_tpb_read);
    if (trflags_ &  details::trf_write)
            flags.push_back((ISC_SCHAR)isc_tpb_write);
    if (trflags_ &  details::trf_read_commited)
            flags.push_back((ISC_SCHAR)isc_tpb_read_committed);
    if (trflags_ & details::trf_rec_version)
            flags.push_back((ISC_SCHAR)isc_tpb_rec_version);
    if (trflags_ &  details::trf_wait)
            flags.push_back((ISC_SCHAR)isc_tpb_wait);
    if (trflags_ &  details::trf_nowait)
            flags.push_back((ISC_SCHAR)isc_tpb_nowait);
    if (trflags_ &  details::trf_concurrency)
            flags.push_back((ISC_SCHAR)isc_tpb_concurrency);
}

void firebird_session_backend::begin()
{
    if (trhp_ == 0)
    {
        std::vector<ISC_SCHAR> local_flags;
        ISC_STATUS stat[stat_size];

        local_flags.push_back((ISC_SCHAR)isc_tpb_version3);
        convert_tr_flags(local_flags);
        if (isc_start_transaction(stat, &trhp_, 1, &dbhp_, local_flags.size(), local_flags.data()))
        {
            throw_iscerror(stat);
        }
    }
}

firebird_session_backend::~firebird_session_backend()
{
    cleanUp();
}

bool firebird_session_backend::is_in_transaction() const SOCI_NOEXCEPT
{
    return trhp_ != 0;
}

bool firebird_session_backend::is_connected()
{
    ISC_STATUS stat[stat_size];
    ISC_SCHAR req[] = { isc_info_ods_version, isc_info_end };
    ISC_SCHAR res[256];

    return isc_database_info(stat, &dbhp_, sizeof(req), req, sizeof(res), res) == 0;
}

void firebird_session_backend::commit_retain()
{
    ISC_STATUS stat[stat_size];

    if (trhp_ != 0)
    {
        if (isc_commit_retaining(stat, &trhp_))
        {
            throw_iscerror(stat);
        }
    }
}

void firebird_session_backend::rollback_retain()
{
    ISC_STATUS stat[stat_size];

    if (trhp_ != 0)
    {
        if (isc_rollback_retaining(stat, &trhp_))
        {
            throw_iscerror(stat);
        }
    }
}

void firebird_session_backend::commit()
{
    ISC_STATUS stat[stat_size];

    if (trhp_ != 0)
    {
        if (isc_commit_transaction(stat, &trhp_))
        {
            throw_iscerror(stat);
        }

        trhp_ = 0;
		trflags_ = details::trf_none;
    }
}

void firebird_session_backend::rollback()
{
    ISC_STATUS stat[stat_size];

    if (trhp_ != 0)
    {
        if (isc_rollback_transaction(stat, &trhp_))
        {
            throw_iscerror(stat);
        }

        trhp_ = 0;
		trflags_ = details::trf_none;
    }
}

isc_tr_handle* firebird_session_backend::current_transaction()
{
    // It will do nothing if we're already inside a transaction.
    begin();

    return &trhp_;
}

void firebird_session_backend::cleanUp()
{
    ISC_STATUS stat[stat_size];

    // at the end of session our transaction is finally commited.
    if (trhp_ != 0)
    {
        if (isc_commit_transaction(stat, &trhp_))
        {
            throw_iscerror(stat);
        }

        trhp_ = 0;
		trflags_ = details::trf_none;
    }

	stop_event_listener();

    if (isc_detach_database(stat, &dbhp_))
    {
        throw_iscerror(stat);
    }

    dbhp_ = 0L;
}

bool firebird_session_backend::get_next_sequence_value(
    session & s, std::string const & sequence, long long & value)
{
    // We could use isq_execute2() directly but this is even simpler.
    s << "select next value for " + sequence + " from rdb$database",
          into(value);

    return true;
}

firebird_statement_backend * firebird_session_backend::make_statement_backend()
{
    return new firebird_statement_backend(*this);
}

details::rowid_backend* firebird_session_backend::make_rowid_backend()
{
    throw soci_error("RowIDs are not supported");
}

firebird_blob_backend * firebird_session_backend::make_blob_backend()
{
    return new firebird_blob_backend(*this);
}

void firebird_session_backend::free_event_buffers()
{
	event_buffer_.clear();
	event_results_.clear();
}

void firebird_session_backend::stop_event_listener()
{
	ISC_LONG handle;
	{
		std::lock_guard lock(event_listener_mutex_);
		handle = event_listen_handle_;
		event_listen_handle_ = 0;
	}

	if (handle)
	{
		ISC_STATUS stat[stat_size];
		const bool failed = isc_cancel_events(stat, &dbhp_, &handle);

		{
			std::lock_guard lock(event_listener_mutex_);
			free_event_buffers();
		}

		if (failed)
		{
			throw_iscerror(stat);
		}
	}
}

class event_iterator
{
	private:
	uint8_t* pos;

	public:
	event_iterator& operator++()
	{
 		pos += 1 + static_cast<int>(*pos) + 4;
		return *this;
	}

	bool operator<(const uint8_t* buf_pos) const
	{
		return pos < buf_pos;
	}

	bool operator==(const uint8_t* buf_pos) const
	{
		return pos == buf_pos;
	}

	std::string get_name() const
	{
		return std::string(pos + 1, pos + 1 + static_cast<int>(*pos));
	}

	uint32_t get_count() const
	{
		return isc_vax_integer(reinterpret_cast<const ISC_SCHAR*>(pos + 1 + *pos), 4);
	}

	void set_count_to(const event_iterator& it)
	{
		std::memcpy(pos + 1 + *pos, it.pos + 1 + *it.pos, 4);
	}

	event_iterator(uint8_t* buf)
	{
		pos = buf + 1;
	}
};


void firebird_session_backend::event_handler(void* object, ISC_USHORT size, const ISC_UCHAR* tmpbuffer)
{
	// >>>>> This method is a STATIC member !! <<<<<
	// Consider this method as a kind of "interrupt handler". It should do as
	// few work as possible as quickly as possible and then return.
	// Never forget: this is called by the Firebird client code, on *some*
	// thread which might not be (and won't probably be) any of your application
	// thread. This function is to be considered as an "interrupt-handler" of a
	// hardware driver.

	// There can be spurious calls to EventHandler from FB internal. We must
	// dismiss those calls.
	if (object == nullptr || size == 0 || tmpbuffer == nullptr)
		return;

	firebird_session_backend* backend = (firebird_session_backend*)object;

	std::lock_guard lock(backend->event_listener_mutex_);
	if (backend->event_listen_handle_)
	{
		std::memcpy(backend->event_results_.data(), tmpbuffer, size);

		event_iterator prev_state = backend->event_buffer_.data();
		for (event_iterator new_state = backend->event_results_.data(); new_state < backend->event_results_.data() + size; ++new_state, ++prev_state)
		{
			const uint32_t prev_num_triggered = prev_state.get_count();
			const uint32_t num_triggered = new_state.get_count();
			if (num_triggered != prev_num_triggered)
			{
				backend->has_events_ = true;
				auto this_event = backend->triggered_events_.try_emplace(new_state.get_name(), 0);
				this_event.first->second += num_triggered - prev_num_triggered;
				prev_state.set_count_to(new_state);
			}
		}

		backend->listen();
	}
}

void firebird_session_backend::listen()
{
	event_listen_handle_ = 0;
	ISC_STATUS stat[stat_size];
	if (isc_que_events(stat, &dbhp_, &event_listen_handle_, event_buffer_.size(), reinterpret_cast<ISC_UCHAR*>(event_buffer_.data()), (ISC_EVENT_CALLBACK)&firebird_session_backend::event_handler, this))
	{
		free_event_buffers();
		throw_iscerror(stat);
	}
}

void firebird_session_backend::trigger_events(std::map<std::string, size_t>& outEvents)
{
	if (has_events_.exchange(false))
	{
		std::lock_guard lock(event_listener_mutex_);
		outEvents.swap(triggered_events_);
	}
}

isc_svc_handle firebird_session_backend::service_connect(const std::string& server, const std::string& user, const std::string& pass)
{
	std::string service_name;
	if (!server.empty())
	{
		service_name = server + ":";
	}

	service_name += "service_mgr";

	ISC_STATUS stat[stat_size];
	std::vector<ISC_SCHAR> spb({isc_spb_version, isc_spb_current_version, isc_spb_user_name});
	spb.push_back(user.length());
	spb.insert(spb.end(), user.begin(), user.end());
	spb.push_back(isc_spb_password);
	spb.push_back(pass.length());
	spb.insert(spb.end(), pass.begin(), pass.end());

	isc_svc_handle service_handle = 0;
	int res = isc_service_attach(stat, service_name.size(), service_name.c_str(), &service_handle, spb.size(), spb.data());
	if (res)
		printf("Service could not be created: %d\n", res);

	return service_handle;
}

void firebird_session_backend::service_disconnect(isc_svc_handle handle)
{
	ISC_STATUS stat[stat_size];
	int res = isc_service_detach(stat, &handle);
	if (res)
		printf("Disconnect problem: %d\n", res);
	/*else
		printf("Disconnect OK!\n");*/
}

int firebird_session_backend::set_db_options(isc_svc_handle handle, const std::string& database_file, const std::vector<ISC_SCHAR>& options)
{
	std::vector<ISC_SCHAR> spb({isc_action_svc_properties, isc_spb_dbname});
	int16_t size = database_file.length();
	size = isc_portable_integer(reinterpret_cast<const ISC_UCHAR*>(&size), 2);
	spb.insert(spb.end(), reinterpret_cast<const ISC_SCHAR*>(&size), reinterpret_cast<const ISC_SCHAR*>(&size) + 2);
	spb.insert(spb.end(), database_file.begin(), database_file.end());
	spb.insert(spb.end(), options.begin(), options.end());

	ISC_STATUS stat[stat_size];
	int ret = isc_service_start(stat, &handle, NULL, spb.size(), spb.data());

	if (ret)
	{
		printf("Service could not be started: %d\n", ret);
		return ret;
	}

	return wait_for_service_result(handle);
}

int firebird_session_backend::wait_for_service_result(isc_svc_handle handle)
{
	std::vector<ISC_SCHAR> spb({isc_info_svc_line});

	for (;;)
	{
		Sleep(1);
		ISC_STATUS stat[stat_size];
		std::array<ISC_SCHAR, 1024> result;
		int ret = isc_service_query(stat, &handle, NULL, 0, NULL, spb.size(), spb.data(), result.size(), result.data());
		if (ret)
		{
			printf("Service query failed: %d\n", ret);
			return ret;
		}

		size_t pos = std::find(result.begin(), result.end(), isc_info_svc_line) - result.begin();
		if (pos >= result.size())
		{
			printf("Bad service query response!\n");
			return -1;
		}

		int str_len = isc_portable_integer(reinterpret_cast<const ISC_UCHAR*>(result.data() + pos + 1), 2);
		if (str_len == 0) // If message length is	zero bytes,	task is	finished
			return 0;
	}
}

void isc_event_block_from_vector(std::vector<uint8_t>& event_buffer, std::vector<uint8_t>& result_buffer, const std::vector<std::string>& events)
{

	// calculate length of event parameter block, setting initial length to include version
	// and counts for each argument
	size_t length = 1;
	for (const std::string& ev : events)
		length += ev.length() + 1 + 4;

	event_buffer.clear();
	result_buffer.clear();
	event_buffer.resize(length, 0);
	result_buffer.resize(length, 0);

	size_t offset = 0;
	event_buffer[offset++] = 1; // EPB_version1

	for(const std::string& ev : events)
	{
		event_buffer[offset++] = ev.length();
		std::memcpy(event_buffer.data() + offset, ev.c_str(), ev.length());
		offset += ev.length() + 4; // 4 bytes for event count
	}
}

bool firebird_session_backend::start_event_listener()
{
	if (event_listen_handle_)
		return false;

	if (registered_events_.empty())
		return false;

	isc_event_block_from_vector(event_buffer_, event_results_, registered_events_);

	if (!event_buffer_.size())
		return false;

	std::lock_guard lock(event_listener_mutex_);
	listen();

	return event_listen_handle_;
}

int firebird_session_backend::set_forced_writes(const std::string& server, const std::string& user, const std::string& pass, const std::string& db_file, bool bSync)
{
    isc_svc_handle service_handle;
    std::vector<ISC_SCHAR> options;
    int res;

    service_handle = service_connect(server, user, pass);
    if (service_handle == 0)
        return -1;

    options.push_back(isc_spb_prp_write_mode);
    options.push_back(bSync ? isc_spb_prp_wm_sync : isc_spb_prp_wm_async);
    res = set_db_options(service_handle, db_file, options);

    service_disconnect(service_handle);
    return res;
}

int firebird_session_backend::set_reserve_space(const std::string& server, const std::string& user, const std::string& pass, const std::string& db_file)
{
    isc_svc_handle service_handle;
    std::vector<ISC_SCHAR> options;
    int res;

    service_handle = service_connect(server, user, pass);
    if (service_handle == 0)
        return -1;

    options.push_back(isc_spb_prp_reserve_space);
    options.push_back(isc_spb_prp_res);
    res = set_db_options(service_handle, db_file, options);

    service_disconnect(service_handle);
    return res;

}
