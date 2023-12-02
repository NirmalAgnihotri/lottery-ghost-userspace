#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> 
#include <unistd.h>

#include "nlohmann/json.hpp"

int main()
{
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == -1)
    {
        perror("socket");
        return 1;
    }

    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(8080);

    // Connect to localhost
    if (inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr) <= 0)
    {
        perror("inet_pton");
        return 1;
    }

    // Connect the socket to the server
    if (connect(clientSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) == -1)
    {
        perror("connect");
        return 1;
    }

    // Send a message
    const char *message = "Hello, Server!";
    ssize_t bytesSent = send(clientSocket, message, strlen(message), 0);

    if (bytesSent == -1)
    {
        perror("send");
        return 1;
    }

    std::cout << "Sent " << bytesSent << " bytes to the server." << std::endl;

    // Close the socket
    close(clientSocket);

    return 0;
}