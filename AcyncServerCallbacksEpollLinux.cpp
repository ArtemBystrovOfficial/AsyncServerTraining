#include <sys/socket.h>  // Для socket, bind, listen, accept
#include <netinet/in.h>  // Для sockaddr_in
#include <arpa/inet.h>   // Для htons и т.д.
#include <unistd.h>      // Для close
#include <fcntl.h>       // Для fcntl
#include <sys/epoll.h>   // Для epoll
#include <cstring>       // Для memset
#include <iostream>      // Для вывода в консоль
#include <thread>        // Для std::thread
#include <mutex>         // Для std::mutex
#include <vector>        // Для std::vector
#include <queue>         // Для std::queue
#include <condition_variable> // Для синхронизации
#include <functional>
#include <map>
#include <queue>

#define MAX_EVENTS 100 //make dynamic after x2
#define MAX_COUNT_FD 10000 //make dynamic after x2
#define PORT 8080

// Глобальная очередь задач

class ThreadPool {
    public:

    using task_t = std::function<void()>;

    void Start(int cnt_workers) {
        for(int i=0;i<cnt_workers;i++) {
            workers_.push_back(std::thread([this]{
                while(true) { //todo stop possible
                    std::unique_lock<std::mutex> lk(mt);
                    cv.wait(lk,[this]{ //todo balance if task before be in other thread move to the same thread
                        return !this->current_tasks_.empty();
                    });

                    auto task = this->current_tasks_.front();
                    this->current_tasks_.pop();

                    //std::cout << "Задача выполнена на потоке: " << std::this_thread::get_id() << std::endl;

                    task();
                }
            }));
            workers_.back().detach();
        }
    }

    void PushTask(task_t task) {
        current_tasks_.push(std::move(task));
        cv.notify_one();
    }
private:
    std::mutex mt;
    std::condition_variable cv;
    std::vector<std::thread> workers_;
    std::queue<task_t> current_tasks_;
};

namespace {
    void set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

class AsyncServer {
public:
     
    using handle_accept_callback_t  = std::function<void(int)>;
    using handle_read_callback_t    = std::function<void(int, std::string)>;
    using handle_write_callback_t   = std::function<void(int, bool)>;

    AsyncServer() {
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ == -1) {
            perror("Epoll creation failed");
            close(epoll_fd_);
            return;
        }
    }

    void WriteAsync(int fd, handle_write_callback_t cb, std::string data) {
        if(!data.empty())
            write_buffer_[fd] = data;
        callback_list_write_[fd] = std::make_unique<handle_write_callback_t>(std::move(cb));

        // Устанавливаем EPOLLOUT
        epoll_event ev;
        ev.events = EPOLLOUT | EPOLLIN; // Теперь следим за записью и чтением
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
            perror("epoll_ctl (EPOLLOUT) failed");
            close(fd);
            return;
        }

    }

    void ReadAsync(int fd, handle_read_callback_t cb) {
        callback_list_read_[fd] = std::make_unique<handle_read_callback_t>(std::move(cb));
    }

    void AcceptAsync(handle_accept_callback_t cb) {
        callback_accept_ = std::make_unique<handle_accept_callback_t>(std::move(cb));
    }

    int Listen(const std::string & ip, int port) { // todo real ip
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ == -1) {
            perror("Socket creation failed");
            return -1;
        }

        set_nonblocking(server_fd_);

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(server_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            perror("Bind failed");
            close(server_fd_);
            return -1;
        }if (listen(server_fd_, SOMAXCONN) == -1) {
            perror("Listen failed");
            close(server_fd_);
            return -1;
        }
        
        epoll_event ev;
        ev.events = EPOLLIN; 
        ev.data.fd = server_fd_;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev) == -1) {
            perror("Epoll add failed");
            close(server_fd_);
            close(epoll_fd_);
            return -1;
        }

        return server_fd_;
    }

    void Run(int thread_count) {
        
        epoll_event events[MAX_EVENTS];

        if(thread_count)
            thread_pool_.Start(thread_count);

        while (true) {
            int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
            if (nfds == -1) {
                perror("Epoll wait failed");
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                auto cl_fd = events[i].data.fd;
                if (cl_fd == server_fd_) {

                    int client_fd = accept(server_fd_, nullptr, nullptr);
                    if (client_fd == -1) {
                        perror("Accept failed");
                        continue;
                    }//std::cout << "New client connected: " << client_fd << "\n";
                    set_nonblocking(client_fd);

                    epoll_event ev;
                    ev.events = EPOLLIN; 
                    ev.data.fd = client_fd;

                    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        perror("Epoll add failed");
                        close(client_fd);
                        close(epoll_fd_);
                        return;
                    }

                    if(callback_accept_) {
                        callback_accept_->operator()(client_fd);
                    }
                }
                if (events[i].events & EPOLLIN) {
                    if(callback_list_read_[cl_fd]) {
                        auto out = ReadHandle(cl_fd);

                        if(!out.empty()) {
                            if(!thread_count) {
                                callback_list_read_[cl_fd]->operator()(cl_fd, std::move(out));
                            }
                            else {
                                auto func = callback_list_read_[cl_fd].release();
                                thread_pool_.PushTask([func=std::move(func), out=std::move(out), cl_fd]{
                                    func->operator()(cl_fd, std::move(out));
                                });
                            }
                        }
                    }
                } 
                if (events[i].events & EPOLLOUT){
                    if(callback_list_write_[cl_fd]) {
                        bool eof = WriteHandle(cl_fd);

                        if(!thread_count) {
                            callback_list_write_[cl_fd]->operator()(cl_fd, eof);
                        }
                        else {
                            auto func = callback_list_write_[cl_fd].release();
                            thread_pool_.PushTask([func=std::move(func),cl_fd, eof]{
                                func->operator()(cl_fd, eof);
                            });
                        }
                    }
                }

            }
        }

        close(server_fd_);
        close(epoll_fd_);
    }


    ~AsyncServer() {
        close(epoll_fd_);
    }

private:

    std::string ReadHandle(int fd) {
        char buffer[1024];
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            return buffer;
        } else if (bytes_read == 0) {
            //std::cout << "Client " << fd << " disconnected.\n";
            close(fd);
        } else {
            perror("Read failed");
            close(fd);
        }
        return "";
    }

    bool WriteHandle(int fd) {
        if(write_buffer_[fd].empty())
            throw std::invalid_argument("bad buffer");

        const std::string& data = write_buffer_[fd];
        ssize_t bytes_written = write(fd, data.c_str(), data.size());if (bytes_written == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Сокет временно не доступен, попробуем снова
                return false;
            } else {
                perror("write failed");
                close(fd);
                return false;
            }
        }

        epoll_event ev;
        ev.events = EPOLLIN; 
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev); //Убираем запись оставляем чтение

        if (bytes_written < data.size()) {
            // Частичная запись: обновляем буфер
            write_buffer_[fd] = data.substr(bytes_written);
            return false;
        } else {
            write_buffer_[fd].clear();
        }
        return true; //todo indise repeat write block of data
    }

    int server_fd_;
    int epoll_fd_;

    std::mutex queue_mutex;
    std::condition_variable cv;
    std::array<std::unique_ptr<handle_read_callback_t>, MAX_COUNT_FD> callback_list_read_;
    std::array<std::unique_ptr<handle_write_callback_t>, MAX_COUNT_FD> callback_list_write_;
    std::array<std::string, MAX_COUNT_FD> write_buffer_;
    //std::unique_ptr<handle_callback_t> callback_list[MAX_COUNT_FD];
    std::unique_ptr<handle_accept_callback_t> callback_accept_;

    ThreadPool thread_pool_;
};



// Рабочий поток для обработки клиентских задач
// Основной серверный поток
void OnRead(AsyncServer & server, int fd);

void OnWrite(AsyncServer & server, int fd, const std::string & msg) {
    server.WriteAsync(fd, [&](int fd, bool is_full_sended) {
        if(!is_full_sended) {
            OnWrite(server,fd,""); //send other data in buffer
        } else {
            //std::cout << "Сообщение отправлено!"<< std::endl;
            OnRead(server, fd);
        }
    }, msg);
}void OnRead(AsyncServer & server, int fd) {
    server.ReadAsync(fd, [&](int fd, std::string data) {
        //std::cout << "data received: " << data << std::endl;
        // Рекурсивно вызываем функцию для продолжения чтения
        //data.pop_back();
        static std::string out = "HTTP/1.1 200 OK\nContent-Type: text/plain\nContent-Length: 13\n\nHello, world!";
        OnWrite(server, fd, out); //echo server
    });
};

int main() {
    AsyncServer server;
    
    server.Listen("0.0.0.0",8080);

    server.AcceptAsync([&](int fd){
        OnRead(server, fd);
    });

    server.Run(std::thread::hardware_concurrency());

    return 0;
}