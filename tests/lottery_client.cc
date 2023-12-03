#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <future>
#include <limits>
#include <algorithm>
#include <tuple>

#include "nlohmann/json.hpp"
using DpTuple = std::tuple<float, int, int>;

std::string createMessage(unsigned int numTickets, std::string payload)
{
    nlohmann::json service_payload;
    service_payload["numTickets"] = 100;
    service_payload["payload"] = payload;
    std::string message_str = service_payload.dump();
    message_str.push_back('\0');
    return message_str;
}

float compute_latency(unsigned int assigned_tickets, unsigned int total_service_tickets, float service_latency)
{
    float proportion = (assigned_tickets * 1.0) / ((assigned_tickets + total_service_tickets) * 1.0);
    return ((1 - proportion) + 1) * service_latency;
}

void print_tuple(DpTuple &tuple)
{
    std::cout << std::get<0>(tuple) << " " << std::get<1>(tuple) << " " << std::get<2>(tuple) << std::endl;
}

std::vector<unsigned int> getOptimalTicketAlloc(const std::vector<float> &service_latencies, const std::vector<unsigned int> &service_tickets, unsigned int t)
{
    unsigned int n = service_latencies.size();

    std::vector<std::vector<DpTuple>> dp(n, std::vector<DpTuple>(t));

    for (int i = t - 1; i >= 0; --i)
    {
        float latency = compute_latency(t - i, service_tickets[n - 1], service_latencies[n - 1]);
        dp[n - 1][i] = std::make_tuple(latency, t - i, -1);
    }

    for (int i = n - 2; i >= 0; --i)
    {
        for (int j = t - 1; j >= 0; --j)
        {
            float min_latency = std::numeric_limits<float>::max();
            unsigned int my_tickets = 1;
            unsigned int my_optimal_tickets = 1;
            int optimal_prev_ptr = -1;
            for (int k = j + 1; k < t; ++k)
            {
                float my_latency = compute_latency(my_tickets, service_tickets[i], service_latencies[i]);
                const DpTuple &prev = dp[i + 1][k];
                float latency_prev = std::get<0>(prev);
                float curr_latency = std::max(my_latency, latency_prev);
                if (curr_latency < min_latency)
                {
                    min_latency = curr_latency;
                    my_optimal_tickets = my_tickets;
                    optimal_prev_ptr = k;
                }

                my_tickets++;
            }
            dp[i][j] = std::make_tuple(min_latency, my_optimal_tickets, optimal_prev_ptr);
        }
    }

    std::vector<unsigned int> ticket_allocs(n);
    int j = 0;
    for (int i = 0; i < ticket_allocs.size(); ++i)
    {
        const DpTuple &curr = dp[i][j];
        ticket_allocs[i] = std::get<1>(curr);
        j = std::get<2>(curr);
    }
    return ticket_allocs;
}

void sendRequest(const char *serverAddress, int port, const char *message, size_t message_len, std::promise<std::string> &&promise)
{
    // Create a socket
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == -1)
    {
        perror("socket");
        promise.set_value("Error creating socket");
        return;
    }

    // Set up server address
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    // Connect to the specified server
    if (inet_pton(AF_INET, serverAddress, &serverAddr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(clientSocket);
        promise.set_value("Error converting IP address");
        return;
    }

    // Connect the socket to the server
    if (connect(clientSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1)
    {
        perror("connect");
        close(clientSocket);
        promise.set_value("Error connecting to server");
        return;
    }

    // Send a message
    ssize_t bytesSent = send(clientSocket, message, message_len, 0);
    if (bytesSent == -1)
    {
        perror("send");
        close(clientSocket);
        promise.set_value("Error sending message");
        return;
    }

    // Receive a response
    char responseBuffer[1024];
    ssize_t bytesRead = recv(clientSocket, responseBuffer, sizeof(responseBuffer), 0);
    if (bytesRead == -1)
    {
        perror("recv");
        close(clientSocket);
        promise.set_value("Error receiving response");
        return;
    }

    // Close the socket
    close(clientSocket);

    // Set the response value
    promise.set_value(std::string(responseBuffer, bytesRead));
}

int main()
{

    const char *server1 = "127.0.0.1";
    const char *server2 = "127.0.0.1";
    std::vector<float> server_latencies;
    server_latencies.push_back(10.0);
    server_latencies.push_back(10.0);
    std::vector<unsigned int> ticket_allocs;
    ticket_allocs.push_back(10);
    ticket_allocs.push_back(5);

    // generate messages
    std::vector<unsigned int> optimal_tickets = getOptimalTicketAlloc(server_latencies, ticket_allocs, 15);
    for (auto i : optimal_tickets)
    {
        std::cout << i << std::endl;
    }

    std::string message1 = createMessage(100, "hello");
    std::string message2 = createMessage(100, "Bye!");

    const char *buffer1 = message1.c_str();
    size_t bufferSize1 = message1.size();
    const char *buffer2 = message2.c_str();
    size_t bufferSize2 = message2.size();

    std::promise<std::string> promise1, promise2;
    std::future<std::string> future1 = promise1.get_future();
    std::future<std::string> future2 = promise2.get_future();

    auto start = std::chrono::steady_clock::now();

    std::thread thread1(sendRequest, server1, 8080, buffer1, bufferSize1, std::move(promise1));
    std::thread thread2(sendRequest, server2, 8082, buffer2, bufferSize2, std::move(promise2));

    // Wait for both threads to finish
    thread1.join();
    thread2.join();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Both requests took " << duration.count() << " milliseconds to finish." << std::endl;

    // Get responses from futures
    std::string response1 = future1.get();
    std::string response2 = future2.get();

    // Handle responses
    std::cout << "Response from Server 1: " << response1 << std::endl;
    std::cout << "Response from Server 2: " << response2 << std::endl;
    return 0;
}