/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee --- CLIPP IronBee Consumer Implementation
 *
 * @author Christopher Alfeld <calfeld@qualys.com>
 */

#include "ironbee.hpp"

#include <clipp/control.hpp>

#include <ironbeepp/all.hpp>
#include <ironbee/rule_engine.h>

#ifdef __clang__
#pragma clang diagnostic push
#if __has_warning("-Wunused-local-typedef")
#pragma clang diagnostic ignored "-Wunused-local-typedef"
#endif
#endif
#include <boost/make_shared.hpp>
#include <boost/thread.hpp>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

using namespace std;

namespace IronBee {
namespace CLIPP {

namespace {

class adapt_header :
    public unary_function<const Input::header_t, IronBee::ParsedHeader>
{
public:
    explicit adapt_header(IronBee::MemoryManager mm) :
        m_mm(mm)
    {
        // nop
    }

    IronBee::ParsedHeader operator()(const Input::header_t& header) const
    {
        return IronBee::ParsedHeader::create(
            m_mm,
            IronBee::ByteString::create_alias(
                m_mm, header.first.data, header.first.length
            ),
            IronBee::ByteString::create_alias(
                m_mm, header.second.data, header.second.length
            )
        );
    }

private:
    IronBee::MemoryManager m_mm;
};

class IronBeeDelegate :
    public Input::Delegate
{
public:
    explicit
    IronBeeDelegate(IronBee::Engine engine) :
        m_engine(engine)
    {
        // nop
    }

    ~IronBeeDelegate()
    {
        if (m_connection) {
            boost::lock_guard<boost::mutex> guard(m_mutex);
            m_connection.destroy();
        }
    }

    void connection_opened(const Input::ConnectionEvent& event)
    {
        {
            boost::lock_guard<boost::mutex> guard(m_mutex);

            if (m_connection) {
                m_connection.destroy();
            }
            m_connection = IronBee::Connection::create(m_engine);

/* Static analyzer doesn't understand register_clean() */
#ifndef __clang_analyzer__
            char* local_ip = strndup(
                event.local_ip.data,
                event.local_ip.length
            );
            m_connection.memory_manager().register_cleanup(boost::bind(free,local_ip));
            char* remote_ip = strndup(
                event.remote_ip.data,
                event.remote_ip.length
            );
            m_connection.memory_manager().register_cleanup(boost::bind(free,remote_ip));

            m_connection.set_local_ip_string(local_ip);
            m_connection.set_local_port(event.local_port);
            m_connection.set_remote_ip_string(remote_ip);
            m_connection.set_remote_port(event.remote_port);
#endif
        }

        m_engine.notify().connection_opened(m_connection);
    }

    void connection_closed(const Input::NullEvent& event)
    {
        if (! m_connection) {
            throw runtime_error(
                "CONNECTION_CLOSED event fired outside "
                "of connection lifetime."
            );
        }
        m_engine.notify().connection_closed(m_connection);

        {
            boost::lock_guard<boost::mutex> guard(m_mutex);
            m_connection.destroy();
        }
        m_connection = IronBee::Connection();
    };

    void connection_data_in(const Input::DataEvent& event)
    {
        throw runtime_error(
            "IronBee no longer supports connection data.  Use @parse."
        );
    }

    void connection_data_out(const Input::DataEvent& event)
    {
        throw runtime_error(
            "IronBee no longer supports connection data.  Use @parse."
        );
    }

    void request_started(const Input::RequestEvent& event)
    {
        if (! m_connection) {
            throw runtime_error(
                "REQUEST_STARTED event fired outside "
                "of connection lifetime."
            );
        }

        if (m_transaction) {
            m_transaction.destroy();
        }
        m_transaction = IronBee::Transaction::create(m_connection);

        IronBee::ParsedRequestLine prl =
            IronBee::ParsedRequestLine::create_alias(
                m_transaction.memory_manager(),
                event.raw.data,      event.raw.length,
                event.method.data,   event.method.length,
                event.uri.data,      event.uri.length,
                event.protocol.data, event.protocol.length
            );

        m_engine.notify().request_started(m_transaction, prl);
    }

    void request_header(const Input::HeaderEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "REQUEST_HEADER_FINISHED event fired outside "
                "of connection lifetime."
            );
        }

        adapt_header adaptor(m_transaction.memory_manager());
        m_engine.notify().request_header_data(
            m_transaction,
            boost::make_transform_iterator(event.headers.begin(), adaptor),
            boost::make_transform_iterator(event.headers.end(),   adaptor)
        );
    }

    void request_header_finished(const Input::NullEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "REQUEST_HEADER_FINISHED event fired outside "
                "of connection lifetime."
            );
        }
        m_engine.notify().request_header_finished(m_transaction);
    }

    void request_body(const Input::DataEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "REQUEST_BODY event fired outside "
                "of connection lifetime."
            );
        }

        // Don't give IronBee empty data.
        if (event.data.length == 0) {
            return;
        }

        m_engine.notify().request_body_data(
            m_transaction,
            event.data.data, event.data.length
        );
    }

    void request_finished(const Input::NullEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "REQUEST_FINISHED event fired outside "
                "of transaction lifetime."
            );
        }
        m_engine.notify().request_finished(m_transaction);
    }

    void response_started(const Input::ResponseEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_STARTED event fired outside "
                "of transaction lifetime."
            );
        }

        IronBee::ParsedResponseLine prl =
            IronBee::ParsedResponseLine::create_alias(
                m_transaction.memory_manager(),
                event.raw.data,      event.raw.length,
                event.protocol.data, event.protocol.length,
                event.status.data,   event.status.length,
                event.message.data,  event.message.length
            );

        m_engine.notify().response_started(m_transaction, prl);
    }

    void response_header(const Input::HeaderEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_HEADER event fired outside "
                "of connection lifetime."
            );
        }

        adapt_header adaptor(m_transaction.memory_manager());
        m_engine.notify().response_header_data(
            m_transaction,
            boost::make_transform_iterator(event.headers.begin(), adaptor),
            boost::make_transform_iterator(event.headers.end(),   adaptor)
        );
    }

    void response_header_finished(const Input::NullEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_HEADER_FINISHED event fired outside "
                "of connection lifetime."
            );
        }
        m_engine.notify().response_header_finished(m_transaction);
    }

    void response_body(const Input::DataEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_BODY event fired outside "
                "of connection lifetime."
            );
        }

        // Don't give IronBee empty data.
        if (event.data.length == 0) {
            return;
        }

        m_engine.notify().response_body_data(
            m_transaction,
            event.data.data, event.data.length
        );
    }

    void response_finished(const Input::NullEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_FINISHED event fired outside "
                "of connection lifetime."
            );
        }

        m_engine.notify().response_finished(m_transaction);
        m_transaction.destroy();
        m_transaction = IronBee::Transaction();
    }

private:
    IronBee::Engine      m_engine;
    IronBee::Connection  m_connection;
    IronBee::Transaction m_transaction;
    boost::mutex         m_mutex;
};

void load_configuration(IronBee::Engine engine, const std::string& path)
{
    IronBee::ConfigurationParser parser
        = IronBee::ConfigurationParser::create(engine);

    engine.configuration_started(parser);

    parser.parse_file(path);

    engine.configuration_finished();

    parser.destroy();
}

enum action_e {
    ACTION_ALLOW,
    ACTION_BLOCK,
    ACTION_BREAK
};

void clipp_action_instance(
    action_e which,
    action_e& to
)
{
    to = which;
}

Action::action_instance_t clipp_action_generator(
    const char* parameters,
    action_e&   to
)
{
    static const string allow_arg("allow");
    static const string block_arg("block");
    static const string break_arg("break");

    if (parameters == allow_arg) {
        return boost::bind(clipp_action_instance, ACTION_ALLOW, boost::ref(to));
    }
    else if (parameters == block_arg) {
        return boost::bind(clipp_action_instance, ACTION_BLOCK, boost::ref(to));
    }
    else if (parameters == break_arg) {
        return boost::bind(clipp_action_instance, ACTION_BREAK, boost::ref(to));
    }
    else {
        BOOST_THROW_EXCEPTION(
            einval() << errinfo_what(
                string("Invalid clipp action: ") + parameters
            )
        );

    }
}

extern "C" {

ib_status_t clipp_error(
    ib_tx_t* tx,
    int      status,
    void*
)
{
    ib_log_error_tx(tx, "clipp_error: %d", status);

    return IB_OK;
}

ib_status_t clipp_header(
    ib_tx_t*                  tx,
    ib_server_direction_t     dir,
    ib_server_header_action_t action,
    const char*               name,
    size_t                    name_length,
    const char*               value,
    size_t                    value_length,
    void*
)
{
    static const char* c_header_actions[] = {
        "set",
        "append",
        "merge",
        "add",
        "unset",
        "edit"
    };

    ib_log_alert_tx(tx,
        "clipp_header: dir=%s action=%s hdr=%.*s value=%.*s",
        (dir == IB_SERVER_REQUEST ? "request" : "response"),
        c_header_actions[action],
        (int)name_length, name, (int)value_length, value
    );

    return IB_OK;
}

ib_status_t clipp_error_body(
    ib_tx_t*           tx,
    const uint8_t*     data,
    size_t             dlen,
    void*
)
{
    ib_log_alert_tx(tx, "clipp_error_body: dlen=%zd\n", dlen);

    return IB_OK;
}

ib_status_t clipp_error_header(
    ib_tx_t*    tx,
    const char* name,
    size_t      name_length,
    const char* value,
    size_t      length,
    void*
)
{
    ib_log_alert_tx(tx, "clipp_error_header: %.*s=%.*s\n",
        (int)name_length, name,
        (int)length, value
    );

    return IB_OK;
}

ib_status_t clipp_close(
    ib_conn_t*,
    ib_tx_t *tx,
    void*
)
{
    ib_log_alert_tx(tx, "clipp_close\n");

    return IB_OK;
}

} // extern "C"

// Move to new file?
template <typename WorkType>
class FunctionWorkerPool :
    private boost::noncopyable
{
    typedef boost::unique_lock<boost::mutex> lock_t;

    void do_work()
    {
        WorkType local_work;
        for (;;) {
            {
                lock_t lock(m_mutex);
                ++m_num_workers_available;

            }
            m_worker_available_cv.notify_one();

            {
                lock_t lock(m_mutex);
                while (! m_work_available) {
                    m_work_available_cv.wait(lock);
                    if (m_shutdown) {
                        return;
                    }
                }

                local_work = m_work;
                m_work_available = false;
                --m_num_workers_available;
            }
            m_work_accepted_barrier.wait();

            m_work_function(local_work);
        }
    }

public:
    FunctionWorkerPool(
        size_t                          num_workers,
        boost::function<void(WorkType)> work_function
    ) :
        m_num_workers(num_workers),
        m_work_function(work_function),
        m_work_accepted_barrier(2),
        m_num_workers_available(0),
        m_work_available(false),
        m_shutdown(false)
    {
        for (size_t i = 0; i < num_workers; ++i) {
            m_thread_group.create_thread(boost::bind(
                &FunctionWorkerPool::do_work,
                this
            ));
        }
    }

    void operator()(WorkType work)
    {
        {
            lock_t lock(m_mutex);

            while (m_num_workers_available == 0) {
                m_worker_available_cv.wait(lock);
            }

            m_work           = work;
            m_work_available = true;

            m_work_available_cv.notify_one();
        }
        m_work_accepted_barrier.wait();
    }

    void shutdown()
    {
        {
            lock_t lock(m_mutex);
            while (m_num_workers_available < m_num_workers) {
                m_worker_available_cv.wait(lock);
            }
        }

        m_shutdown = true;
        m_work_available_cv.notify_all();

        m_thread_group.join_all();
    }

private:
    size_t                          m_num_workers;
    boost::function<void(WorkType)> m_work_function;

    boost::mutex                    m_mutex;
    boost::condition_variable       m_worker_available_cv;
    boost::condition_variable       m_work_available_cv;
    boost::barrier                  m_work_accepted_barrier;
    boost::thread_group             m_thread_group;
    size_t                          m_num_workers_available;
    bool                            m_work_available;
    bool                            m_shutdown;
    WorkType                        m_work;
};

} // Anonymous

struct IronBeeConsumer::State
{
    boost::function<bool(Input::input_p&)> modifier;
};

IronBeeConsumer::IronBeeConsumer(const string& config_path) :
    m_state(boost::make_shared<State>())
{
    m_state->modifier = IronBeeModifier(config_path);
}

bool IronBeeConsumer::operator()(const Input::input_p& input)
{
    // Insist the input not modified.
    Input::input_p copy = input;
    m_state->modifier(copy);

    return true;
}

struct IronBeeModifier::State
{
    State() :
        behavior(ALLOW),
        current_action(ACTION_ALLOW),
        server_value(__FILE__, "clipp")
    {
        IronBee::initialize();
        engine = IronBee::Engine::create(server_value.get());
    }

    ~State()
    {
        engine.destroy();
        IronBee::shutdown();
    }

    behavior_e           behavior;
    action_e             current_action;
    IronBee::Engine      engine;
    IronBee::ServerValue server_value;
};

IronBeeModifier::IronBeeModifier(
    const string& config_path,
    behavior_e    behavior
) :
    m_state(boost::make_shared<State>())
{
    m_state->behavior = behavior;

    m_state->server_value.get().ib()->hdr_fn = clipp_header;
    m_state->server_value.get().ib()->err_fn = clipp_error;
    m_state->server_value.get().ib()->err_hdr_fn = clipp_error_header;
    m_state->server_value.get().ib()->err_body_fn = clipp_error_body;
    m_state->server_value.get().ib()->close_fn = clipp_close;

    Action::create(
        m_state->engine.main_memory_mm(),
        "clipp",
        boost::bind(
            clipp_action_generator,
            _3,
            boost::ref(m_state->current_action)
        )
    ).register_with(m_state->engine);

    load_configuration(m_state->engine, config_path);
}

bool IronBeeModifier::operator()(Input::input_p& input)
{
    if (! input) {
        return true;
    }

    IronBeeDelegate delegate(m_state->engine);

    switch (m_state->behavior) {
        case ALLOW: m_state->current_action = ACTION_ALLOW;  break;
        case BLOCK: m_state->current_action = ACTION_BLOCK; break;
        default:
            throw logic_error("Unknown behavior.  Please report as bug.");
    }

    input->connection.dispatch(delegate, true);

    switch (m_state->current_action) {
        case ACTION_ALLOW: return true;
        case ACTION_BLOCK: return false;
        case ACTION_BREAK: throw clipp_break();
        default:
            throw logic_error("Unknown clipp action.  Please report as bug.");
    }
}

struct IronBeeThreadedConsumer::State
{
    void process_input(Input::input_p input)
    {
        if (! input) {
            return;
        }

        IronBeeDelegate delegate(engine);
        input->connection.dispatch(delegate, true);
    }

    explicit
    State(size_t num_workers) :
        worker_pool(
            num_workers,
            boost::bind(
                &IronBeeThreadedConsumer::State::process_input,
                this,
                _1
            )
        ),
        server_value(__FILE__, "clipp")

    {
        IronBee::initialize();
        engine = IronBee::Engine::create(server_value.get());
    }

    ~State()
    {
        worker_pool.shutdown();

        engine.destroy();
        IronBee::shutdown();
    }


    FunctionWorkerPool<Input::input_p> worker_pool;
    IronBee::Engine      engine;
    IronBee::ServerValue server_value;
};

IronBeeThreadedConsumer::IronBeeThreadedConsumer(
    const string& config_path,
    size_t        num_workers
) :
    m_state(boost::make_shared<State>(num_workers))
{
    load_configuration(m_state->engine, config_path);
}

bool IronBeeThreadedConsumer::operator()(const Input::input_p& input)
{
    m_state->worker_pool(input);

    return true;
}

} // CLIPP
} // IronBee
