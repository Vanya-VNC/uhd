//
// Copyright 2010-2013 Ettus Research LLC
// Copyright 2018 Ettus Research, a National Instruments Company
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <boost/thread/condition.hpp>
#include <boost/thread/thread.hpp>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace po   = boost::program_options;
namespace asio = boost::asio;

typedef std::shared_ptr<asio::ip::udp::socket> socket_type;

static const size_t insane_mtu = 9000;

#if defined(UHD_PLATFORM_MACOS)
// limit buffer resize on macos or it will error
const size_t rx_dsp_buff_size = size_t(1e6);
#else
// set to half-a-second of buffering at max rate
const size_t rx_dsp_buff_size = size_t(50e6);
#endif

const size_t tx_dsp_buff_size = (1 << 20);

/***********************************************************************
 * Signal handlers
 **********************************************************************/
static bool stop_signal_called = false;
void sig_int_handler(int)
{
    stop_signal_called = true;
}

static bool wait_for_recv_ready(int sock_fd)
{
    // setup timeval for timeout
    timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 100000; // 100ms

    // setup rset for timeout
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock_fd, &rset);

    // call select with timeout on receive socket
    return ::select(sock_fd + 1, &rset, NULL, NULL, &tv) > 0;
}

/***********************************************************************
 * Relay class
 **********************************************************************/
class udp_relay_type
{
public:
    udp_relay_type(const std::string& server_addr,
        const std::string& client_addr,
        const std::string& port,
        const size_t server_rx_size = 0,
        const size_t server_tx_size = 0,
        const size_t client_rx_size = 0,
        const size_t client_tx_size = 0)
        : _port(port)
    {
        {
            asio::ip::udp::resolver resolver(_io_context);
            asio::ip::udp::endpoint endpoint =
                *resolver.resolve(asio::ip::udp::v4(), server_addr, port).begin();

            _server_socket = std::shared_ptr<asio::ip::udp::socket>(
                new asio::ip::udp::socket(_io_context, endpoint));
            resize_buffs(_server_socket, server_rx_size, server_tx_size);
        }
        {
            asio::ip::udp::resolver resolver(_io_context);
            asio::ip::udp::endpoint endpoint =
                *resolver.resolve(asio::ip::udp::v4(), client_addr, port).begin();

            _client_socket = std::shared_ptr<asio::ip::udp::socket>(
                new asio::ip::udp::socket(_io_context));
            _client_socket->open(asio::ip::udp::v4());
            _client_socket->connect(endpoint);
            resize_buffs(_client_socket, client_rx_size, client_tx_size);
        }

        std::cout << "spawning relay threads... " << _port << std::endl;
        std::unique_lock<std::mutex> lock(
            spawn_mutex); // lock in preparation to wait for threads to spawn
        (void)_thread_group.create_thread(
            std::bind(&udp_relay_type::server_thread, this));
        wait_for_thread.wait(lock); // wait for thread to spin up
        (void)_thread_group.create_thread(
            std::bind(&udp_relay_type::client_thread, this));
        wait_for_thread.wait(lock); // wait for thread to spin up
        std::cout << "    done!" << std::endl << std::endl;
    }

    ~udp_relay_type(void)
    {
        std::cout << "killing relay threads... " << _port << std::endl;
        _thread_group.interrupt_all();
        _thread_group.join_all();
        std::cout << "    done!" << std::endl << std::endl;
    }

private:
    static void resize_buffs(socket_type sock, const size_t rx_size, const size_t tx_size)
    {
        if (rx_size != 0)
            sock->set_option(asio::socket_base::receive_buffer_size(rx_size));
        if (tx_size != 0)
            sock->set_option(asio::socket_base::send_buffer_size(tx_size));
    }

    void server_thread(void)
    {
        std::cout << "    entering server_thread..." << std::endl;
        wait_for_thread.notify_one(); // notify constructor that this thread has started
        std::vector<char> buff(insane_mtu);
        while (not boost::this_thread::interruption_requested()) {
            if (wait_for_recv_ready(_server_socket->native_handle())) {
                std::unique_lock<std::mutex> lock(_endpoint_mutex);
                const size_t len = _server_socket->receive_from(
                    asio::buffer(&buff.front(), buff.size()), _endpoint);
                lock.unlock();
                _client_socket->send(asio::buffer(&buff.front(), len));

                // perform sequence error detection on tx dsp data (can detect bad network
                // cards)
                /*
                if (_port[4] == '7'){
                    static uint32_t next_seq;
                    const uint32_t this_seq = ntohl(reinterpret_cast<const uint32_t
                *>(&buff.front())[0]); if (next_seq != this_seq and this_seq != 0)
                std::cout << "S" << std::flush; next_seq = this_seq + 1;
                }
                */
            }
        }
        std::cout << "    exiting server_thread..." << std::endl;
    }

    void client_thread(void)
    {
        std::cout << "    entering client_thread..." << std::endl;
        wait_for_thread.notify_one(); // notify constructor that this thread has started
        std::vector<char> buff(insane_mtu);
        while (not boost::this_thread::interruption_requested()) {
            if (wait_for_recv_ready(_client_socket->native_handle())) {
                const size_t len =
                    _client_socket->receive(asio::buffer(&buff.front(), buff.size()));
                std::lock_guard<std::mutex> lock(_endpoint_mutex);
                _server_socket->send_to(asio::buffer(&buff.front(), len), _endpoint);
            }
        }
        std::cout << "    exiting client_thread..." << std::endl;
    }

    const std::string _port;
    boost::thread_group _thread_group;
    asio::io_context _io_context;
    asio::ip::udp::endpoint _endpoint;
    std::mutex _endpoint_mutex;
    socket_type _server_socket, _client_socket;
    std::mutex spawn_mutex;
    boost::condition wait_for_thread;
};


/***********************************************************************
 * Main
 **********************************************************************/
int UHD_SAFE_MAIN(int argc, char* argv[])
{
    // variables to be set by po
    std::string addr;
    std::string bind;

    // setup the program options
    po::options_description desc("Allowed options");
    // clang-format off
    desc.add_options()
        ("help", "help message")
        ("addr", po::value<std::string>(&addr), "the resolvable address of the usrp (must be specified)")
        ("bind", po::value<std::string>(&bind)->default_value("0.0.0.0"), "bind the server to this network address (default: any)")
    ;
    // clang-format on
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    // print the help message
    if (vm.count("help") or not vm.count("addr")) {
        std::cout << "UHD Network Relay " << desc << std::endl
                  << "Runs a network relay between UHD on one computer and a USRP on the "
                     "network.\n"
                  << "This example is basically for test purposes. Use at your own "
                     "convenience.\n"
                  << std::endl;
        return EXIT_FAILURE;
    }

    {
        std::shared_ptr<udp_relay_type> ctrl(new udp_relay_type(bind, addr, "49152"));
        std::shared_ptr<udp_relay_type> rxdsp0(new udp_relay_type(
            bind, addr, "49156", 0, tx_dsp_buff_size, rx_dsp_buff_size, 0));
        std::shared_ptr<udp_relay_type> txdsp0(new udp_relay_type(
            bind, addr, "49157", tx_dsp_buff_size, 0, 0, tx_dsp_buff_size));
        std::shared_ptr<udp_relay_type> rxdsp1(new udp_relay_type(
            bind, addr, "49158", 0, tx_dsp_buff_size, rx_dsp_buff_size, 0));
        std::shared_ptr<udp_relay_type> gps(new udp_relay_type(bind, addr, "49172"));

        std::signal(SIGINT, &sig_int_handler);
        std::cout << "Press Ctrl + C to stop streaming..." << std::endl;

        while (not stop_signal_called) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // finished
    std::cout << std::endl << "Done!" << std::endl << std::endl;

    return EXIT_SUCCESS;
}
