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
        void handleClient(int clientSocket)
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

                if (jsonData.contains("exampleKey"))
                {
                    std::string value = jsonData["exampleKey"];
                    std::cout << "Value of exampleKey: " << value << std::endl;
                }
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

        int startServer(int serverSocket, const std::unique_ptr<PrioTable> &table_)
        {
            // Bind the socket to an IP address and port
            sockaddr_in serverAddress{};
            serverAddress.sin_family = AF_INET;
            serverAddress.sin_addr.s_addr = INADDR_ANY; // Bind to any available network interface
            serverAddress.sin_port = htons(8080);       // Use port 8080

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
                    new GhostThread(GhostThread::KernelScheduler::kGhost, [&]
                                    { handleClient(clientSocket); }));
            }

            // Close the server socket (not reached in this example)
            close(serverSocket);

            // Join all threads before exiting
            for (auto &thread : threads)
            {
                thread->Join();
            }
        }

    }
}

int main()
{
    const int kPrioMax = 51200;

    auto table_ = std::make_unique<ghost::PrioTable>(
        kPrioMax, 3,
        ghost::PrioTable::StreamCapacity::kStreamCapacity19);
    // Create a socket
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1)
    {
        std::cerr << "Error creating socket\n";
        return -1;
    }

    {
        if (ghost::startServer(serverSocket, table_) == -1)
        {
            return -1;
        }
    }
}
