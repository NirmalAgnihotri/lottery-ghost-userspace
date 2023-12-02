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
            virtual int handle(std::string payload) = 0;
        };

        class ServiceA : public ServiceHandler
        {
        public:
            int handle(std::string payload) override
            {
                return 1;
            }
        };

        class ServiceB : public ServiceHandler
        {
        public:
            int handle(std::string payload) override
            {
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

            void handleClient(int port, int clientSocket, unsigned int prio_id, const std::unique_ptr<PrioTable> &table_, std::unordered_map<unsigned int, Gtid> &gtid_map)
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
                    unsigned int num_tickets = jsonData["numTickets"].get<unsigned int>();
                    setPriority(table_, num_tickets, prio_id, gtid_map);
                    auto it = handlers.find(port);
                    it->second->handle(jsonData["payload"]);
                }
                catch (const nlohmann::json::exception &e)
                {
                    std::cerr << "Error parsing JSON: " << e.what() << std::endl;
                }

                // Use the mutex to protect cout
                std::lock_guard<std::mutex> lock(mtx);
                std::cout << "Received from client: " << buffer << "\n";

                // Close the client socket
                close(clientSocket);
            }

        public:
            void registerServiceHandler(int port, std::unique_ptr<ServiceHandler> handler)
            {
                handlers[port] = std::move(handler);
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
                serverAddress.sin_port = htons(8080); // Use port 8080

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

                std::cout << "Server listening on port 8080...\n";

                std::vector<std::unique_ptr<ghost::GhostThread>> threads;
                threads.reserve(1000);

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

                    // Create a new thread to handle the client
                    threads.emplace_back(
                        new GhostThread(GhostThread::KernelScheduler::kGhost, [&, clientSocket, prio_start]
                                        { handleClient(port, clientSocket, prio_start, table_, prio_to_gtid); }));
                    auto &t = threads[prio_start];
                    UpdateSchedItem(table_.get(), prio_start, t->gtid(), 10000);
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
    server.registerServiceHandler(8080, std::make_unique<ghost::ServiceA>());
    server.registerServiceHandler(8082, std::make_unique<ghost::ServiceB>());
    const int kPrioMax = 51200;

    auto table_ = std::make_unique<ghost::PrioTable>(
        kPrioMax, 3,
        ghost::PrioTable::StreamCapacity::kStreamCapacity19);
    server.startServer(port, table_);
}
