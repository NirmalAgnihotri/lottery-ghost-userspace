#include <iostream>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>
#include <mutex>
#include <unistd.h>
#include <arpa/inet.h>

#include "lib/base.h"
#include "lib/ghost.h"
#include "shared/prio_table.h"
#include "nlohmann/json.hpp"

std::mutex mtx; // Mutex for protecting cout in case multiple threads write to it simultaneously
namespace ghost
{
    namespace
    {
        class ServiceHandler
        {
        public:
            virtual int handle(int clientSocket, std::string payload) = 0;
        };

        class ServiceA : public ServiceHandler
        {
        public:
            int handle(int clientSocket, std::string payload) override
            {
                unsigned long long g = 0;
                auto start = std::chrono::steady_clock::now();

                for (int i = 0; i < std::pow(10, 7); i += 27)
                {
                    g += i * (i - 1) * (i + 1);
                    // absl::SleepFor(absl::Milliseconds(1));
                }
                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                std::cout << "Service A took " << duration.count() << " milliseconds to finish."
                          << " with value " << g << std::endl;
                const char *message = "A Done!";
                send(clientSocket, message, strlen(message), 0);
                return 1;
            }
        };

        class ServiceB : public ServiceHandler
        {
        public:
            int handle(int clientSocket, std::string payload) override
            {
                unsigned long long g = 0;
                auto start = std::chrono::steady_clock::now();

                for (int i = 0; i < std::pow(10, 9); i += 27)
                {
                    g += i * (i - 1) * (i + 1);
                    // absl::SleepFor(absl::Milliseconds(1));
                }
                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                std::cout << "Service B took " << duration.count() << " milliseconds to finish."
                          << " with value " << g << std::endl;
                const char *message = "B Done!";
                send(clientSocket, message, strlen(message), 0);
                return 1;
            }
        };

        class Server
        {
        private:
            std::unordered_map<int, std::unique_ptr<ServiceHandler>> handlers;

            void
            UpdateSchedItem(PrioTable *table, uint32_t sidx, const Gtid &gtid, int num_tickets)
            {
                struct sched_item *si;
                std::cout << "sid " << sidx << " gtid " << gtid << " " << num_tickets << std::endl;

                si = table->sched_item(sidx);

                const uint32_t seq = si->seqcount.write_begin();
                si->sid = sidx;
                si->gpid = gtid.id();
                si->deadline = num_tickets;
                si->seqcount.write_end(seq);
                table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
            }

            void setPriority(const std::unique_ptr<PrioTable> &table_, unsigned int num_tickets, unsigned int prio_id, std::unordered_map<unsigned int, Gtid> &gtid_map)
            {
                UpdateSchedItem(table_.get(), prio_id, gtid_map[prio_id], num_tickets);
            }

            void handleClient(int port, int clientSocket, int service_id, std::string payload)
            {
                auto it = handlers.find(service_id);
                it->second->handle(clientSocket, payload);
            }

            nlohmann::json parseClientData(int clientSocket)
            {
                const int buffer_size = 1024;
                char buffer[buffer_size] = {0};
                std::string receivedData;

                while (true)
                {
                    int bytesRead = recv(clientSocket, buffer, buffer_size, 0);

                    if (bytesRead <= 0)
                    {
                        // Handle connection closure or error
                        break;
                    }

                    // Append the received data to the string
                    receivedData.append(buffer, bytesRead);

                    // Check if the end of the JSON string is received
                    if (receivedData.find('\0') != std::string::npos)
                    {
                        break; // End of the JSON string
                    }
                }

                // Parse JSON from the buffer
                try
                {
                    nlohmann::json jsonData = nlohmann::json::parse(receivedData);

                    std::cout << "Parsed JSON:\n"
                              << jsonData.dump(2) << std::endl;
                    return jsonData;
                }
                catch (const nlohmann::json::exception &e)
                {
                    std::cerr << "Error parsing JSON: " << e.what() << std::endl;
                    return nullptr;
                }
            }

        public:
            void registerServiceHandler(int serviceId, std::unique_ptr<ServiceHandler> handler)
            {
                handlers[serviceId] = std::move(handler);
            }

            int startServer(int port, const std::unique_ptr<PrioTable> &table_)
            {
                unsigned int prio_start = 0;
                std::unordered_map<unsigned int, Gtid> prio_to_gtid;

                int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
                if (serverSocket == -1)
                {
                    std::cerr << "Error creating socket\n";
                    return -1;
                }

                // Bind the socket to an IP address and port
                sockaddr_in serverAddress{};
                serverAddress.sin_family = AF_INET;
                serverAddress.sin_port = htons(port); // Use port 8080

                if (inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr) <= 0)
                {
                    perror("inet_pton");
                    return 1;
                }

                if (bind(serverSocket, reinterpret_cast<sockaddr *>(&serverAddress), sizeof(serverAddress)) == -1)
                {
                    std::cerr << "Error binding socket\n";
                    close(serverSocket);
                    return -1;
                }

                // Listen for incoming connections
                if (listen(serverSocket, 10) == -1)
                {
                    std::cerr << "Error listening for connections\n";
                    close(serverSocket);
                    return -1;
                }

                std::cout << "Server listening on port " << port << "...\n";

                std::vector<std::unique_ptr<ghost::GhostThread>> threads;
                threads.reserve(1000);

                for (int i = 0; i < 2; ++i)
                {
                    threads.emplace_back(new GhostThread(GhostThread::KernelScheduler::kGhost, [&]
                                                         {
                                                            unsigned long long g = 0;

                                                            for (int i = 0; i < std::pow(10, 11); i += 27)
                                                            {
                                                                g += i * (i - 1) * (i + 1);
                                                            }
                                                            std::cout << g << std::endl; }));
                }
                for (int i = 0; i < 2; ++i)
                {
                    auto &t = threads[i];
                    UpdateSchedItem(table_.get(), prio_start + i, t->gtid(), (i + 1) * 1000);
                }

                prio_start += 2;

                while (true)
                {
                    // Accept connections
                    sockaddr_in clientAddress{};
                    socklen_t clientAddrSize = sizeof(clientAddress);

                    int clientSocket = accept(serverSocket, reinterpret_cast<sockaddr *>(&clientAddress), &clientAddrSize);
                    if (clientSocket == -1)
                    {
                        std::cerr << "Error accepting connection\n";
                        continue; // Try accepting the next connection
                    }

                    std::cout << "Connection accepted from " << inet_ntoa(clientAddress.sin_addr) << ":" << ntohs(clientAddress.sin_port) << "\n";
                    nlohmann::json clientData = parseClientData(clientSocket);
                    unsigned int num_tickets = clientData["numTickets"].get<unsigned int>();
                    int service_id = clientData["serviceId"].get<int>();
                    std::string payload = clientData["payload"];

                    threads.emplace_back(
                        new GhostThread(GhostThread::KernelScheduler::kGhost, [&, clientSocket, service_id, payload]
                                        { handleClient(port, clientSocket, service_id, payload); }));
                    auto &t = threads[prio_start];
                    UpdateSchedItem(table_.get(), prio_start, t->gtid(), num_tickets);
                    prio_to_gtid[prio_start] = t->gtid();
                    prio_start++;
                }

                // Close the server socket (not reached in this example)
                close(serverSocket);

                // Join all threads before exiting
                for (auto &thread : threads)
                {
                    thread->Join();
                }
            }
        };
    };
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }
    int port = std::stoi(argv[1]);
    ghost::Server server;
    server.registerServiceHandler(1, std::make_unique<ghost::ServiceA>());
    server.registerServiceHandler(2, std::make_unique<ghost::ServiceB>());
    const int kPrioMax = 51200;

    auto table_ = std::make_unique<ghost::PrioTable>(
        kPrioMax, 3,
        ghost::PrioTable::StreamCapacity::kStreamCapacity19);
    server.startServer(port, table_);
}
