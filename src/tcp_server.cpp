#include <fcntl.h>          
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/signal.h>
#include <sys/wait.h>

#include <cstring>         
#include <cstdlib> 


#include <string>
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <functional>
#include <atomic>


#include "tcp_server.hpp"
#include "openssl.hpp"



namespace mongols {

    std::atomic_bool tcp_server::done(true);
    int tcp_server::backlog = 511;

    void tcp_server::signal_normal_cb(int sig, siginfo_t *, void *) {
        switch (sig) {
            case SIGTERM:
            case SIGQUIT:
            case SIGINT:
                tcp_server::done = false;
                break;
            default:break;
        }
    }

    tcp_server::tcp_server(const std::string& host
            , int port
            , int timeout
            , size_t buffer_size
            , int max_event_size) :
    host(host), port(port), listenfd(0), max_event_size(max_event_size), serveraddr()
    , buffer_size(buffer_size), thread_size(0), sid(0), timeout(timeout), sid_queue(), clients(), work_pool(0)
    , openssl_manager(), openssl_crt_file(), openssl_key_file(), openssl_is_ok(false) {
        this->listenfd = socket(AF_INET, SOCK_STREAM, 0);

        int on = 1;
        setsockopt(this->listenfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof (on));

        struct timeval send_timeout, recv_timeout;
        send_timeout.tv_sec = this->timeout;
        send_timeout.tv_usec = 0;
        setsockopt(this->listenfd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof (send_timeout));

        recv_timeout.tv_sec = this->timeout;
        recv_timeout.tv_usec = 0;
        setsockopt(this->listenfd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof (recv_timeout));



        memset(&this->serveraddr, 0, sizeof (this->serveraddr));
        this->serveraddr.sin_family = AF_INET;
        inet_aton(this->host.c_str(), &serveraddr.sin_addr);
        this->serveraddr.sin_port = htons(this->port);
        bind(this->listenfd, (struct sockaddr*) & this->serveraddr, sizeof (this->serveraddr));

        this->setnonblocking(this->listenfd);

        listen(this->listenfd, tcp_server::backlog);

    }

    tcp_server::~tcp_server() {
        if (this->work_pool) {
            delete this->work_pool;
        }
    }

    tcp_server::client_t::client_t() : ip(), port(-1), t(time(0)), sid(0), uid(0), u_size(0), count(0), gid() {
        this->gid.push_back(0);
    }

    tcp_server::client_t::client_t(const std::string& ip, int port, size_t uid, size_t gid)
    : ip(ip), port(port), t(time(0)), sid(0), uid(uid), u_size(0), count(0), gid() {
        this->gid.push_back(gid);
    }

    tcp_server::meta_data_t::meta_data_t() : client(), ssl() {

    }

    tcp_server::meta_data_t::meta_data_t(const std::string& ip, int port, size_t uid, size_t gid)
    : client(ip, port, uid, gid), ssl() {

    }

    void tcp_server::run(const handler_function& g) {
        std::vector<int> sigs = {SIGTERM, SIGINT, SIGQUIT};

        struct sigaction act;
        for (size_t i = 0; i < sigs.size(); ++i) {
            memset(&act, 0, sizeof (struct sigaction));
            sigemptyset(&act.sa_mask);
            act.sa_sigaction = tcp_server::signal_normal_cb;
            act.sa_flags = SA_SIGINFO;
            if (sigaction(sigs[i], &act, NULL) < 0) {
                perror("sigaction error");
                return;
            }
        }
        mongols::epoll epoll(this->max_event_size, -1);
        if (!epoll.is_ready()) {
            perror("epoll error");
            return;
        }
        epoll.add(this->listenfd, EPOLLIN | EPOLLET);
        auto main_fun = std::bind(&tcp_server::main_loop, this, std::placeholders::_1, std::cref(g), std::ref(epoll));
        if (this->thread_size > 0) {
            this->work_pool = new mongols::thread_pool < std::function<bool() >>(this->thread_size);
        }
        while (tcp_server::done) {
            epoll.loop(main_fun);
        }
    }

    void tcp_server::setnonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    bool tcp_server::add_client(int fd, const std::string& ip, int port) {
        auto pair = this->clients.insert(std::move(std::make_pair(fd, std::move(meta_data_t(ip, port, 0, 0)))));
        if (this->sid_queue.empty()) {
            pair.first->second.client.sid = ++this->sid;
        } else {
            pair.first->second.client.sid = this->sid_queue.front();
            this->sid_queue.pop();
        }
        if (this->openssl_is_ok) {
            pair.first->second.ssl = std::make_shared<openssl::ssl>(this->openssl_manager->get_ctx());
            if (!this->openssl_manager->set_socket_and_accept(pair.first->second.ssl->get_ssl(), fd)) {
                return false;
            }
        }
        return true;

    }

    void tcp_server::del_client(int fd) {
        this->sid_queue.push(this->clients.find(fd)->second.client.sid);
        this->clients.erase(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    bool tcp_server::send_to_all_client(int fd, const std::string& str, const filter_handler_function& h) {
        for (auto i = this->clients.begin(); i != this->clients.end();) {
            if (i->first != fd && h(i->second.client) &&
                    (this->openssl_is_ok
                    ? this->openssl_manager->write(i->second.ssl->get_ssl(), str) < 0
                    : send(i->first, str.c_str(), str.size(), MSG_NOSIGNAL) < 0)
                    ) {
                this->del_client(i->first);
            } else {
                ++i;
            }
        }
        return false;
    }

    bool tcp_server::work(int fd, const handler_function& g) {
        char buffer[this->buffer_size];
        bool rereaded = false;
ev_recv:
        ssize_t ret = recv(fd, buffer, this->buffer_size, MSG_WAITALL);
        if (ret < 0) {
            if (errno == EINTR) {
                if (!rereaded) {
                    rereaded = true;
                    goto ev_recv;
                }
            } else if (errno == EAGAIN) {
                return false;
            }
            goto ev_error;

        } else if (ret > 0) {
            std::pair<char*, size_t> input;
            input.first = &buffer[0];
            input.second = ret;
            filter_handler_function send_to_other_filter = [](const client_t&) {
                return true;
            };

            bool keepalive = CLOSE_CONNECTION, send_to_all = false;
            client_t& client = this->clients[fd].client;
            client.u_size = this->clients.size();
            client.count++;
            std::string output = std::move(g(input, keepalive, send_to_all, client, send_to_other_filter));
            ret = send(fd, output.c_str(), output.size(), MSG_NOSIGNAL);
            if (ret > 0) {
                if (send_to_all) {
                    this->send_to_all_client(fd, output, send_to_other_filter);
                }
            }

            if (ret <= 0 || keepalive == CLOSE_CONNECTION) {
                goto ev_error;
            }

        } else {

ev_error:
            this->del_client(fd);
        }

        return false;
    }

    bool tcp_server::ssl_work(int fd, const handler_function& g) {
        char buffer[this->buffer_size];
        ssize_t ret = 0;
        std::shared_ptr<openssl::ssl> ssl = this->clients[fd].ssl;
        bool rereaded = false;
ev_recv:
        ret = this->openssl_manager->read(ssl->get_ssl(), buffer, this->buffer_size);
        if (ret < 0) {
            int err = SSL_get_error(ssl->get_ssl(), ret);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return false;
            } else if (err == SSL_ERROR_SYSCALL) {
                if (errno == EINTR) {
                    if (!rereaded) {
                        rereaded = true;
                        goto ev_recv;
                    }
                } else if (errno == EAGAIN) {
                    return false;
                }
            }
            goto ev_error;

        } else if (ret > 0) {
            std::pair<char*, size_t> input;
            input.first = &buffer[0];
            input.second = ret;
            filter_handler_function send_to_other_filter = [](const client_t&) {
                return true;
            };

            bool keepalive = CLOSE_CONNECTION, send_to_all = false;
            client_t& client = this->clients[fd].client;
            client.u_size = this->clients.size();
            client.count++;
            std::string output = std::move(g(input, keepalive, send_to_all, client, send_to_other_filter));

            ret = this->openssl_manager->write(ssl->get_ssl(), output);
            if (ret > 0) {
                if (send_to_all) {
                    this->send_to_all_client(fd, output, send_to_other_filter);
                }
            }

            if (ret <= 0 || keepalive == CLOSE_CONNECTION) {
                goto ev_error;
            }

        } else {

ev_error:
            this->del_client(fd);

        }

        return false;
    }

    void tcp_server::main_loop(struct epoll_event * event
            , const handler_function& g
            , mongols::epoll& epoll) {
        if (event->data.fd == this->listenfd) {
            while (tcp_server::done) {
                struct sockaddr_in clientaddr;
                socklen_t clilen;
                int connfd = accept(listenfd, (struct sockaddr*) &clientaddr, &clilen);
                if (connfd > 0) {
                    epoll.add(connfd, EPOLLIN | EPOLLRDHUP | EPOLLET);
                    if (!this->add_client(connfd, inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port))) {
                        this->del_client(connfd);
                        break;
                    }
                    this->setnonblocking(connfd);
                } else {
                    break;
                }
            }
        } else if (event->events & EPOLLIN) {
            if (this->openssl_is_ok) {
                this->ssl_work(event->data.fd, g);
            } else {
                this->work(event->data.fd, g);
            }
        } else {
            this->del_client(event->data.fd);
        }
    }

    size_t tcp_server::get_buffer_size() const {
        return this->buffer_size;
    }

    bool tcp_server::set_openssl(const std::string& crt_file, const std::string& key_file
            , openssl::version_t v
            , const std::string& ciphers
            , long flags) {
        this->openssl_crt_file = crt_file;
        this->openssl_key_file = key_file;
        this->openssl_manager = std::move(std::make_shared<mongols::openssl>(this->openssl_crt_file, this->openssl_key_file, v, ciphers, flags));
        this->openssl_is_ok = this->openssl_manager && this->openssl_manager->is_ok();
        return this->openssl_is_ok;
    }




}