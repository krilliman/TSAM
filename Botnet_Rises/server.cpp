//
// Simple chat server for TSAM-409
//
// Command line: ./chat_server 4000 
//
// Author: Jacky Mallett (jacky@ru.is)
//
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>
#include <fcntl.h>
#include <stack>

#include <iostream>
#include <sstream>
#include <thread>
#include <map>
#include "ip.cpp"
#include <unistd.h>

// fix SOCK_NONBLOCK for OSX
#ifndef SOCK_NONBLOCK
#include <fcntl.h>
#define SOCK_NONBLOCK O_NONBLOCK
#endif

#define BACKLOG  5          // Allowed length of queue of waiting connections

// Simple class for handling connections from clients.
//
// Client(int socket) - socket to send/receive traffic from client.
class Client
{
  public:
    int sock;              // socket of client connection
    std::string name;           // Limit length of name of client's user

    Client(int socket) : sock(socket){} 

    ~Client(){}            // Virtual destructor defined for base class
};

class Server
{
public:
    int sock;              // socket of client connection
    std::string name;           // Limit length of name of client's user

    Server(int socket) : sock(socket){}

    ~Server(){}            // Virtual destructor defined for base class
};

void handleConnection(const char* ipAddress, const char* port, int listenServersPort);
void handleListServer(int socket, int listenServersPort, bool incomingConnection);
void handleConnection(const char* ipAddress, const char* port, int listenServersPort);
void serverList(int socket, std::string groupName, char *buffer);


// Note: map is not necessarily the most efficient method to use here,
// especially for a server with large numbers of simulataneous connections,
// where performance is also expected to be an issue.
//
// Quite often a simple array can be used as a lookup table, 
// (indexed on socket no.) sacrificing memory for speed.

//std::map<int, Client*> clients; // Lookup table for per Client information
//std::map<int, Server*> servers;
// Open socket for specified port.
//
// Returns -1 if unable to create the socket for any reason.
fd_set openSockets;
std::map<std::string, int> serversSockets;
std::map<std::string, std::pair<std::string, int>> serversByGroupId;
std::map<int, std::string> nameByPort;
std::map<std::pair<std::string,int>,int> leaveMap;
std::map<int, Client*> clients;         // Lookup table for per Client information
std::map<int, Server*> servers;         // Lookup table for per Server information
std::string myName;

int open_socket(int portno)
{
   struct sockaddr_in sk_addr;   // address settings for bind()
   int sock;                     // socket opened for this port
   int set = 1;                  // for setsockopt

   // Create socket for connection. Set to be non-blocking, so recv will
   // return immediately if there isn't anything waiting to be read.
#ifdef __APPLE__     
   if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
   {
      perror("Failed to open socket");
      return(-1);
   }
#else
   if((sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
   {
        perror("Failed to open socket");
        return(-1);
   }
#endif

   // Turn on SO_REUSEADDR to allow socket to be quickly reused after 
   // program exit.

   if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
   {
        perror("Failed to set SO_REUSEADDR:");
   }
   set = 1;
#ifdef __APPLE__     
   if(setsockopt(sock, SOL_SOCKET, SOCK_NONBLOCK, &set, sizeof(set)) < 0)
   {
     perror("Failed to set SOCK_NOBBLOCK");
   }
#endif
   memset(&sk_addr, 0, sizeof(sk_addr));

   sk_addr.sin_family      = AF_INET;
   sk_addr.sin_addr.s_addr = INADDR_ANY;
   sk_addr.sin_port        = htons(portno);

   // Bind to socket to listen for connections from clients

   if(bind(sock, (struct sockaddr *)&sk_addr, sizeof(sk_addr)) < 0)
   {
      perror("Failed to bind to socket:");
      return(-1);
   }
   else
   {
      return(sock);
   }
}

// Close a client's connection, remove it from the client list, and
// tidy up select sockets afterwards.

void closeClient(int clientSocket, fd_set *openSockets, int *maxfds, std::map<int, Client*> clients)
{
     // Remove client from the clients list
     clients.erase(clientSocket);

     // If this client's socket is maxfds then the next lowest
     // one has to be determined. Socket fd's can be reused by the Kernel,
     // so there aren't any nice ways to do this.

     if(*maxfds == clientSocket)
     {
        for(auto const& p : clients)
        {
            *maxfds = std::max(*maxfds, p.second->sock);
        }
     }

     // And remove from the list of open sockets.

     FD_CLR(clientSocket, openSockets);
}
void closeServer(int serverSocket, int *maxfds)
{
    // Remove client from the clients list
    std::string groupName = nameByPort.find(serverSocket)->second;
    std::cout << "groupName: " << groupName << std::endl;
    serversSockets.erase(groupName);
    serversByGroupId.erase(groupName);
    nameByPort.erase(serverSocket);
    servers.erase(serverSocket);

    // If this client's socket is maxfds then the next lowest
    // one has to be determined. Socket fd's can be reused by the Kernel,
    // so there aren't any nice ways to do this.

    if(*maxfds == serverSocket)
    {
        for(auto const& p : servers)
        {
            *maxfds = std::max(*maxfds, p.second->sock);
        }
    }
    std::cout << "disconnecting from " << groupName << std::endl;
    // And remove from the list of open sockets.

    FD_CLR(serverSocket, &openSockets);
}
// Process command from client on the server

void clientCommand(int clientSocket, fd_set *openSockets, int *maxfds, char *buffer)
{
  std::vector<std::string> tokens;
  std::string token;

  // Split command from client into tokens for parsing
  std::stringstream stream(buffer);

  while(stream >> token)
      tokens.push_back(token);

  if((tokens[0].compare("SENDMSG") == 0) && (tokens.size() == 3))
  {
     if(tokens[1] == myName) //Checking if the message was meant for this server or not
     {
            std::cout << "This message was sent to me and the message is: " << tokens[2] << std::endl;
            
     }
     else 
     {
        int socket = 0;
        socket = serversSockets.find(tokens[1])->second; // if it finds the connection on our serverlist
        if(socket != 0)
        {
            std::cout << "Need to implement message function here";
        }
        else // Sends to our servers to see if they have the server or not.
        {
            std::cout << "I'm not connected to that group.";
        }
     }
  }
  else if(tokens[0].compare("LISTSERVERS") == 0)
  {
    serverList(clientSocket,myName,buffer);
    // Reducing the msg length by 1 loses the excess "," - which
    // granted is totally cheating.
    // This is slightly fragile, since it's relying on the order
    // of evaluation of the if statement.
  }
  else
  {
    std::cout << "Unknown command from client:" << buffer << std::endl;
  }    

}
void serverList(int socket, std::string groupName, char *buffer)
{
    std::string msg;
    int sendSocket;
    sendSocket = serversSockets.find(groupName)->second;
    if(groupName == myName)
    {
        for(auto const & p : serversByGroupId){
            msg += p.first + ", " + p.second.first + ", " + std::to_string(p.second.second) + "; ";
        }
        if(msg.length() == 0)
        {
            std::cout << "There are no server connected to me." << std::endl;
            msg = "The server list is empty ";
        }
        send(socket, msg.c_str(), msg.length()-1, 0);
    }
    else if(sendSocket != 0)
    {
        char response[1025];
        int nread;
        send(sendSocket, buffer, strlen(buffer), 0);
        nread = read(sendSocket,response,strlen(response));
        send(socket, response, strlen(response),0);
    }
    else
    {
        msg = "I am not connected to that server, please try a different name. ";
        send(socket, msg.c_str(), msg.length()-1, 0);
    }
}

void serverCommand(int serverSocket, fd_set *openSockets, int *maxfds, char *buffer)
{
    std::cout << "entering serverCommand" << std::endl;
    std::vector<std::string> tokens;
    std::string token;

    // Split command from client into tokens for parsing
    std::stringstream stream(buffer);

    while(stream >> token)
        tokens.push_back(token);

    if((tokens[0].compare("01") != 0) || (tokens[tokens.size()-1].compare("04")) != 0)
    {
        printf("Invalid command format, <01> <Command>,< comma separated parameters > <04>\n");
        return;
    }
    else if((tokens[1].compare("SERVERS")) == 0){
        serverList(serverSocket,tokens[1],buffer);
    }

}

void handleServers(int listenServerSock, int serverPort, int *maxfds)
{
    bool finished;
    //fd_set openSockets;             // Current open sockets
    fd_set readSockets;             // Socket list for select()
    fd_set exceptSockets;           // Exception socket list
    int serverSock;                 // Socket of connecting client
    struct sockaddr_in server;
    socklen_t serverLen;
    char buffer[1025];              // buffer for reading from clients

    printf("Listening on port: %d for Servers\n", serverPort);
    std::cout << "test after listenServerSock: " << listenServerSock << std::endl;
    if(listen(listenServerSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %s\n", serverPort);
        exit(0);
    }
    else
        // Add listen socket to socket set we are monitoring
    {
        FD_SET(listenServerSock, &openSockets);
        *maxfds = listenServerSock;

    }
    finished = false;

    while(!finished) {
        // Get modifiable copy of readSockets
        readSockets = exceptSockets = openSockets;
        memset(buffer, 0, sizeof(buffer));
        // Look at sockets and see which ones have something to be read()
        int n = select(*maxfds + 1, &readSockets, NULL, &exceptSockets, NULL);

        if (n < 0) {
            perror("select failed - closing down\n");
            finished = true;
        } else {
            // First, accept  any new connections to the server on the listening socket
            if (FD_ISSET(listenServerSock,
                         &readSockets))                              // Tests to se if listenSock is part of readSockets
            {
                serverSock = accept(listenServerSock, (struct sockaddr *) &server,
                                    &serverLen);                                        // Extracts the first connection request on the queue of pending connections
                printf("accept***\n");                                          // returns a new fd referring to that socket
                // Add new client to the list of open sockets
                FD_SET(serverSock, &openSockets);

                // And update the maximum file descriptor
                *maxfds = std::max(*maxfds, serverSock);

                // create a new server to store information.
                servers[serverSock] = new Server(serverSock);
                // this is just to fill in the maps for the incoming server
                // could be a better way but since we need the get the server name
                // i have no other idea how to get it
                //handleListServer(serverSock, 0, true);
                // ToDo need to figure out a way to do this properly
                // ToDo when i did this like above we got the error of sending 01 SERVERS 04 between 2 servers
                // Decrement the number of sockets waiting to be dealt with
                n--;
                printf("Servers connected on server: %d\n", servers.size());
                //printf("size of map: %d \n", servers.size());
            }
            while(n-- > 0)
            {
                for(auto const& pair : servers)
                {
                    Server *tmpServer = pair.second;

                    if(FD_ISSET(tmpServer->sock, &readSockets))
                    {
                        // recv() == 0 means client has closed connection
                        if(recv(tmpServer->sock, buffer, sizeof(buffer), MSG_DONTWAIT) == 0)
                        {
                            printf("Client closed connection: %d", tmpServer->sock);
                            close(tmpServer->sock);

                            closeServer(tmpServer->sock, maxfds);

                        }
                            // We don't check for -1 (nothing received) because select()
                            // only triggers if there is something on the socket for us.
                        else
                        {
                            //std::cout << buffer << std::endl;
                            serverCommand(tmpServer->sock, &openSockets, maxfds,
                                         buffer);
                        }
                    }
                }
            }
        }
    }

}
void handleClients(int listenClientSock, int clientPort, int *maxfds)
{
    bool finished;
    fd_set openSockets;             // Current open sockets
    fd_set readSockets;             // Socket list for select()
    fd_set exceptSockets;           // Exception socket list
    //int maxfds;                     // Passed to select() as max fd in set
    int clientSock;                 // Socket of connecting client
    struct sockaddr_in client;
    socklen_t clientLen;
    char buffer[1025];              // buffer for reading from clients

    printf("Listening on port: %d for clients\n", clientPort);
    if(listen(listenClientSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %s\n", clientPort);
        exit(0);
    }
    else
        // Add listen socket to socket set we are monitoring
    {
        FD_ZERO(&openSockets);
        FD_SET(listenClientSock, &openSockets);
        *maxfds = listenClientSock;
    }

    finished = false;

    while(!finished)
    {
        // Get modifiable copy of readSockets
        readSockets = exceptSockets = openSockets;
        memset(buffer, 0, sizeof(buffer));

        // Look at sockets and see which ones have something to be read()
        int n = select(*maxfds + 1, &readSockets, NULL, &exceptSockets, NULL);

        if(n < 0)
        {
            perror("select failed - closing down\n");
            finished = true;
        }
        else
        {
            // First, accept  any new connections to the server on the listening socket
            if(FD_ISSET(listenClientSock, &readSockets))                              // Tests to se if listenSock is part of readSockets
            {
                clientSock = accept(listenClientSock, (struct sockaddr *)&client,
                                    &clientLen);                                        // Extracts the first connection request on the queue of pending connections
                printf("accept***\n");                                          // returns a new fd referring to that socket
                // Add new client to the list of open sockets
                FD_SET(clientSock, &openSockets);

                // And update the maximum file descriptor
                *maxfds = std::max(*maxfds, clientSock) ;

                // create a new client to store information.
                clients[clientSock] = new Client(clientSock);

                // Decrement the number of sockets waiting to be dealt with
                n--;

                printf("Client connected on server: %d\n", clients.size());
                //printf("size of map: %d \n", clients.size());
            }
            // Now check for commands from clients
            while(n-- > 0)
            {
                for(auto const& pair : clients)
                {
                    Client *client = pair.second;

                    if(FD_ISSET(client->sock, &readSockets))
                    {
                        // recv() == 0 means client has closed connection
                        if(recv(client->sock, buffer, sizeof(buffer), MSG_DONTWAIT) == 0)
                        {
                            printf("Client closed connection: %d", client->sock);
                            close(client->sock);

                            closeClient(client->sock, &openSockets, maxfds, clients);

                        }
                            // We don't check for -1 (nothing received) because select()
                            // only triggers if there is something on the socket for us.
                        else
                        {
                            std::cout << buffer << std::endl;
                            clientCommand(client->sock, &openSockets, maxfds,
                                          buffer);
                        }
                    }
                }
            }
        }
    }



}

void localServerCommand(const char* buffer, int serverPort, int &maxfds)
{
    std::vector<std::string> tokens;
    std::string token;

    // Split command into tokens for parsing
    std::stringstream stream(buffer);

    while(stream >> token)
        tokens.push_back(token);

    if((tokens[0].compare("01") != 0) || (tokens[tokens.size()-1].compare("04")) != 0)
    {
        printf("Invalid command format, <01> <Command>,< comma separated parameters > <04>\n");
        return;
    }
    else if((tokens[1].compare("LEAVE") == 0))
    {
        // for now lets just have everything space separated
        // just so we can get the functionality working
        if(tokens.size() != 5){
            printf("Invalid format of LEAVE <01> LEAVE, IP, Port <04>");
        }
        std::string ip = tokens[2];
        int port = stoi(tokens[3]);
        if(leaveMap.find(std::make_pair(ip,port)) == leaveMap.end()){
            std::cout << "pair <" << ip << "," << port << "> not found in map" << std::endl;
        }
        int sockToDisconnect = leaveMap.find(std::make_pair(ip,port))->second;
        if(FD_ISSET(sockToDisconnect, &openSockets))
        {
            int closeVal = close(sockToDisconnect);
            if(closeVal != -1){
                std::cout << "closeval: " << closeVal << std::endl;
                closeServer(sockToDisconnect, &maxfds);
            }
        }
        else{
            std::cout << "socket " << sockToDisconnect << " not found" << std::endl;
        }
        for(auto const& p : leaveMap){
            std::cout << "p.first.first " << p.first.first <<  std::endl;
            std::cout << "p.first.second " << p.first.second <<  std::endl;
            std::cout << "p.second " << p.second <<  std::endl << std::endl;
        }

        /*
        std::cout << "sock: " << sockToDisconnect  << std::endl;
        for(auto const& p : leaveMap){
            std::cout << "p.first.first " << p.first.first << std::endl;
            std::cout << "p.first.second " << p.first.second << std::endl;
            std::cout << "p.second " << p.second << std::endl;
            std::cout << "" << std::endl;
        }
         */
    }
    else if((tokens[1].compare("CONNECTLOCAL") == 0))
    {
        std::cout << "connection to local port " << tokens[2] << std::endl;
        handleConnection("127.0.0.1", tokens[2].c_str(), serverPort);

        std::cout << "" << std::endl;
        std::cout << "CURRENT SERVERS" << std::endl;
        std::cout << "" << std::endl;
        for(auto const& p : serversByGroupId){
            std::cout << "p.first: " << p.first << std::endl;
            std::cout << "p.second.first: " << p.second.first << std::endl;
            std::cout << "p.second.second: " << p.second.second << std::endl;
            std::cout << "" << std::endl;
        }
    }
    else{
        std::cout << "no command found, TOKEN: " << tokens[1] << std::endl;
    }

}

void handleListServer(int socket, int listenServersPort, bool incomingConnection)
{
    int nwrite;
    int nread;
    char buffer[1025];
    bzero(buffer, sizeof(buffer));
    strcpy(buffer, "01 SERVERS 04");
    std::cout << "buffer before send " << buffer << std::endl;
    nwrite = send(socket, buffer, strlen(buffer), 0);
    memset(buffer, 0, sizeof(buffer));
    nread = read(socket, buffer, sizeof(buffer));
    std::cout << "buffer before read " << buffer << std::endl;
    std::cout << "nread " << nread << std::endl;

    std::stringstream stream(buffer);
    std::string tmp;
    std::stack<std::string> s;
    bool firstFound = false;

    std::cout << "buffer: " << buffer << std::endl;
    while(stream >> tmp){
        std::cout << "tmp: " << tmp << std::endl;
        char lastOne = tmp[tmp.length()-1];
        std::string newVal = tmp.substr(0,tmp.length()-1);
        if(lastOne == ';'){
            std::string ip = s.top();
            s.pop();
            std::string groupName = s.top();
            s.pop();
            std::cout << " "  << std::endl;
            std::cout << "IP: " << ip << ", port: " << newVal << std::endl;
            std::cout << " "  << std::endl;
            if(!firstFound){
                int port = stoi(newVal);
                serversByGroupId.insert(std::make_pair(groupName, std::make_pair(ip, port)));
                serversSockets.insert(std::make_pair(groupName, socket));
                leaveMap.insert(std::make_pair(std::make_pair(ip, port), socket));
                nameByPort.insert(std::make_pair(socket, groupName));
                servers[socket] = new Server(socket);
                firstFound = true;
                if(incomingConnection){
                    //if incoming only check the first servers info
                    break;
                }
            }
            else {
                //should not be needed since break; above
                if (!incomingConnection) {
                    std::cout << "IP: " << ip << ", port: " << newVal << std::endl;
                    handleConnection(ip.c_str(), newVal.c_str(), listenServersPort);
                }
            }
        }
        else{
            s.push(newVal);
        }
    }
}

void handleConnection(const char* ipAddress, const char* port, int listenServersPort)
{
    if(serversByGroupId.size() > 4){
        return;
    }
    struct sockaddr_in serv_Addr;
    struct sockaddr_in sk_addr;   // address settings for bind()
    struct hostent *server;
    int tmpSocket;
    int set = 1;
    server = gethostbyname(ipAddress);
    bzero((char*) &serv_Addr, sizeof(serv_Addr));
    serv_Addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serv_Addr.sin_addr.s_addr, server->h_length);
    serv_Addr.sin_port = htons(atoi(port));

    tmpSocket = socket(AF_INET, SOCK_STREAM , 0);
    if(setsockopt(tmpSocket, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
    {
        perror("setsockopt failed: ");
    }
    memset(&sk_addr, 0, sizeof(sk_addr));

    sk_addr.sin_family      = AF_INET;
    sk_addr.sin_addr.s_addr = INADDR_ANY;
    sk_addr.sin_port        = htons(listenServersPort);

    // Bind to socket to listen for connections from clients

    if(bind(tmpSocket, (struct sockaddr *)&sk_addr, sizeof(sk_addr)) < 0)
    {
        perror("Failed to bind to socket:");
        //exit(0);
    }

    if(connect(tmpSocket, (struct sockaddr*)&serv_Addr, sizeof(serv_Addr)) < 0)
    {
        std::cout << "failed to connect to server at: " << ntohs(serv_Addr.sin_addr.s_addr) << std::endl;
        perror("could not connect ");
        //exit(0);
    }

    /*
     * ToDo need to process the message and insert to the std::map<std::string, int> serversSockets
     * ToDo the right values
     */

    handleListServer(tmpSocket, listenServersPort, false);
    FD_ZERO(&openSockets);
    FD_SET(tmpSocket, &openSockets);
}

int main(int argc, char* argv[])
{
    if(argc != 5)
    {
        printf("Usage: chat_server <groupname port destIp destPort>\n");
        exit(0);
    }
    /*
     * Todo 1. make the file expect more parameters ./server <portListen><serverToConnectTo><serverPort>
     * Todo 2. after you get a connection send the LISTSERVERS,<FROM GROUP ID>  command to the server
     * Todo 3. begin with inserting the info of the first parameter info the networkInfo (the server that you connected to)
     * Todo 4. after that try to connect to the rest of the servers that he is connected to,
     * Todo remember to make a check for the group_ID so you dont try to make a connection twice
     * Todo 5. when a server successfully connects to your server do the do step 2 and 3 again
     */
    int listenClientSock;                   // Socket for connections to server
    int listenServerSock;                   // Socket for server connections
    int serverPort = atoi(argv[2]);
    int clientPort = 10000;
    char buffer[1025];
    //fd_set openSockets;
    int maxfds;                     // Passed to select() as max fd in set
    std::map<std::string, std::string> networkInfo;
    findMyIp(networkInfo);

    listenServerSock = open_socket(serverPort);                     // Open the socket for the server connections
    myName = argv[1];
    handleConnection(argv[3], argv[4], serverPort);

    std::map<std::string, std::string>::const_iterator pos = networkInfo.find("eth1");
    std::string groupName(argv[1]);
    if(pos != networkInfo.end()){
        serversByGroupId.insert(std::make_pair(groupName, std::make_pair(pos->second, serverPort)));
    }
    else{
        printf("eth1 not found\n");
    }

    // Setup socket for server to listen to

    listenClientSock = open_socket(clientPort);                     // Open the socket for the client connections


    for(auto const& p : serversByGroupId){
        std::cout << "p first " << p.first << "  p.second.first " << p.second.first << "  p.second.second " << p.second.second << std::endl;
    }

    std::thread clientThread(handleClients, listenClientSock, clientPort, &maxfds);
    std::thread serverThread(handleServers,listenServerSock, serverPort, &maxfds);
    //handleServers(listenServerSock, serverPort, servers, serversByGroupId);

    /*
     * ToDo Here should come a while loop for the handling the messages
     * ToDo to other servers.
     */

    for(auto const& p : serversSockets){
        std::cout << "p.first" << p.first << " p.second " << p.second << std::endl;
    }

    bool finished = false;

    while(!finished){
        bzero(buffer, sizeof(buffer));
        fgets(buffer, sizeof(buffer), stdin);
        localServerCommand(buffer, serverPort, maxfds);

        /*
        std::stringstream sstream(buffer);
        std::string token;
        std::vector<std::string> tokens;
        while(sstream >> token){
            tokens.push_back(token);
        }
        if((tokens[0].compare("CONNECTLOCAL") == 0))
        {
            std::cout << "connection to local port " << tokens[1] << std::endl;
            handleConnection("127.0.0.1", tokens[1].c_str(), serverPort);
        }
        else{
            std::cout << "no command found, TOKEN: " << tokens[0] << std::endl;
        }
        std::cout << "" << std::endl;
        std::cout << "CURRENT SERVERS" << std::endl;
        std::cout << "" << std::endl;
        for(auto const& p : serversByGroupId){
            std::cout << "p.first: " << p.first << std::endl;
            std::cout << "p.second.first: " << p.second.first << std::endl;
            std::cout << "p.second.second: " << p.second.second << std::endl;
            std::cout << "" << std::endl;
        }
        */
        /*
        int testSock = serversSockets.find("P3_GROUP65_TEST")->second;
        int nwrite = send(testSock, buffer, strlen(buffer),0);

        if(nwrite  == -1)
        {
            perror("send() to server failed: ");
            finished = true;
        }
        */
    }
    serverThread.join();
    clientThread.join();

    return 0;
}
