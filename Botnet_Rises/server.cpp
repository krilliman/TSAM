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
#include <mutex>

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
    std::string name = "";           // Limit length of name of client's user
    std::string ip = "";
    int port = 0;
    int keepAliveMsg = 0;           //  remove this later
    int keepAliveMsgMax = 0;        //  remove this later
    bool checkedIn = false;
    Server(int socket) : sock(socket){}

    ~Server(){}            // Virtual destructor defined for base class
};

void handleConnection(const char* ipAddress, const char* port, int listenServersPort, int *maxfds);
void handleListServer(int socket, int listenServersPort, bool incomingConnection, int *maxfds);
void handleConnection(const char* ipAddress, const char* port, int listenServersPort, int *maxfds);
void serverList(int socket, std::string groupName, char *buffer);
void sendMSG(std::string groupName, const char *msg);
void getMSG(int socket, std::string groupName);
std::vector<std::string> split(const std::string& s, char delimiter);
bool check(std::string check);   

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
std::map<std::pair<std::string,int>,int> leaveMap;
std::map<int, Client*> clients;         // Lookup table for per Client information
std::map<int, Server*> servers;         // Lookup table for per Server information
std::string myName;
std::map<std::string, std::vector<std::string>> serverMessages;
std::map<std::string, std::vector<std::string>> messagesToBeSent;

Server *currentServer = new Server(0);

// mutex for maps
std::mutex serverMutex;             //6
std::mutex serversSocketsMutex;      //2
std::mutex serversByGroupIdMutex;   //3
std::mutex leaveMapMutex;           //4
std::mutex serverMessagesMutex;     //1
std::mutex messegesToBeSentMutex;        //5


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
    //std::string groupName = nameByPort.find(serverSocket)->second;
    if(servers.find(serverSocket) == servers.end()){
        std::cout << "server not found " << std::endl;
    }
    std::string groupName = servers.find(serverSocket)->second->name;
    auto pp = std::make_pair(servers.find(serverSocket)->second->ip, servers.find(serverSocket)->second->port);

    serversSocketsMutex.lock();
    serversSockets.erase(groupName);
    serversSocketsMutex.unlock();
    serversByGroupIdMutex.lock();
    serversByGroupId.erase(groupName);
    serversByGroupIdMutex.unlock();
    servers.erase(serverSocket);
    leaveMapMutex.lock();
    leaveMap.erase(pp);
    leaveMapMutex.unlock();

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
    std::string text = buffer;
    std::vector<std::string> tokens = split(text,',');

    if((tokens[0].compare("SENDMSG") == 0))
    {
        std::string msg;
        for(int i = 2; i < tokens.size(); i++){
            msg += tokens[i];
        }
        if(tokens[1] == myName) //Checking if the message was meant for this server or not
        {
            serverMessagesMutex.lock();
            auto pos = serverMessages.find(myName);
            if(pos != serverMessages.end()){
                pos->second.push_back(msg);
            }
            else
            {
                std::vector<std::string> tmpVector;
                tmpVector.push_back(msg);
                serverMessages.insert(std::make_pair(myName, tmpVector));
            }
            serverMessagesMutex.unlock();
        }
        else
        {
            sendMSG(tokens[1], msg.c_str());
        }
    }
    else if(tokens[0].compare("GETMSG") == 0)
    {
        getMSG(clientSocket, tokens[1]);
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
    std::string msg = currentServer->name + "," + currentServer->ip + "," + std::to_string(currentServer->port) + "; ";
    serversSocketsMutex.lock();
    int sendSocket;
    sendSocket = serversSockets.find(groupName)->second;
    serversSocketsMutex.unlock();
    if(groupName == myName)
    {
        serversByGroupIdMutex.lock();
        for(auto const & p : serversByGroupId){
            msg += p.first + "," + p.second.first + "," + std::to_string(p.second.second) + "; ";
        }
        serversByGroupIdMutex.unlock();
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
std::vector<std::string> split(const std::string& s, char delimiter)
{
    std::string text = s;
    std::string test1 = text.substr(0,2);
    std::string test2 = text.substr(text.length()-2,text.length());
    if(test1 == "01" && test2 == "04") // Checks if the command is from a server or not
    {
        text = text.substr(2, text.length()-4);
    }

    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(text);

   while(std::getline(tokenStream, token, delimiter))
   {
        if(token[0] == ' ')
        {
            token = token.substr(1,token.size());
        }
        tokens.push_back(token);
   }

    std::string tmp = tokens[tokens.size()-1];
    if( tmp[tmp.length()-1] == '\n')
    {
        tmp = tmp.substr(0,tmp.length()-1);
        tokens[tokens.size()-1] = tmp;
    }
   return tokens;
}
bool check(std::string check)
{
    std::string test1 = check.substr(0,2);
    std::string test2 = check.substr(check.length()-2,check.length());

    if(test1 == "01" && test2 == "04")
    {
        return true;
    }
    else
        return false;
}
void getMSG(int socket, std::string groupName)
{
    std::string msg;
    serverMessagesMutex.lock();
    auto pos = serverMessages.find(groupName);
    if(pos != serverMessages.end())
    {
        msg = pos->second.front();
        pos->second.erase(pos->second.begin());
        send(socket, msg.c_str(),msg.length(),0);
        return;
    }
    serverMessagesMutex.unlock();
    msg = "I don't have any messages from this server.";
    send(socket, msg.c_str(),msg.length(),0);
}
void emptyMessagesToBeSent(std::string groupName, int serverSocket)
{
    messegesToBeSentMutex.lock();
    auto pos = messagesToBeSent.find(groupName);
    if(pos == messagesToBeSent.end()){
        std::string msg = "01SENDMSG," + myName + "," + groupName + ",No messages found04";
        send(serverSocket, msg.c_str(), msg.length(), 0);
        messegesToBeSentMutex.unlock();
        return;
    }
    for(auto s : pos->second){
        sendMSG(groupName, s.c_str());
        struct timeval tv = {10, 0};   // sleep for ten minutes!
        int timeout = select(0, NULL, NULL, NULL, &tv);
    }
    messegesToBeSentMutex.unlock();
}
void sendMSG(std::string groupName, const char *msg)
{
    serversSocketsMutex.lock();
    auto pos = serversSockets.find(groupName);
    if(pos == serversSockets.end()){
        messegesToBeSentMutex.lock();
        auto toBeSentPos = messagesToBeSent.find(groupName);
        if(toBeSentPos == messagesToBeSent.end()){
            std::vector<std::string> tmpVector;
            std::string s = msg;
            tmpVector.push_back(s);
            messagesToBeSent.insert(std::make_pair(groupName, tmpVector));
        }
        else{
            toBeSentPos->second.push_back(std::to_string(*msg));
        }
        messegesToBeSentMutex.unlock();
        std::cout << "server with this group ID was not found " << std::endl;
        serversSocketsMutex.unlock();
        return;
    }
    serversSocketsMutex.unlock();
    int socket = pos->second;

    std::string buffer = "01SENDMSG," + myName + "," + groupName + "," + msg + "04";
    int bSent = send(socket, buffer.c_str(), strlen(buffer.c_str()), 0);
    if(bSent > 0){
        std::cout << "message send successfully" << std::endl;
    }
    else{
        std::cout << "send message failed" << std::endl;
    }
}
void serverCommand(int serverSocket, int *maxfds, char *buffer)
{
    std::string text = buffer;
    std::vector<std::string> tokens = split(text,',');

    if(!check(text))
    {
        printf("Invalid command format, <01><Command>,<comma separated parameters><04>\n");
        return;
    }
    else if((tokens[0].compare("SERVERS")) == 0){
        serverList(serverSocket,myName,buffer);
    }
    else if((tokens[0].compare("LISTSERVERS")) == 0){
        serverList(serverSocket,tokens[1],buffer);
    }
    else if((tokens[0].compare("SENDMSG")) == 0){
        serverMessagesMutex.lock();
        auto pos = serverMessages.find(tokens[1]);
        if(pos == serverMessages.end()){
            std::vector<std::string> tmpVector;
            tmpVector.push_back(tokens[3]);
            serverMessages.insert(std::make_pair(tokens[1], tmpVector));
        }
        else{
            pos->second.push_back(tokens[3]);
        }
        serverMessagesMutex.unlock();
        std::cout << "message from " << tokens[1] << " " << tokens[3] << std::endl;
    }
    else if(tokens[0].compare("KEEPALIVE") == 0){
        if(tokens.size() != 2){
            printf("Invalid command format, format: <01><KEEPALIVE>,<No, messages><04>\n");
            return;
        }
        servers[serverSocket]->checkedIn = true;
        int msg = stoi(tokens[1]);
        if(msg > 0){
            std::string getmsg = "01GETMSG," + myName + "04";
            send(serverSocket, getmsg.c_str(), getmsg.length(), 0);
        }
    }
    else if(tokens[0].compare("GETMSG") == 0){
        if(tokens.size() != 2){
            printf("Invalid command format, format: <01><GETMSG>,<GROUP ID><04>\n");
            return;
        }
        emptyMessagesToBeSent(tokens[1], serverSocket);
    }
    else if(tokens[0].compare("STATUSREQ") == 0){
        if(tokens.size() != 2){
            printf("Invalid command format, format: <01> <STATUSREQ>,<FROM GROUP> <04>\n");
            return;
        }
        std::string msg = "01STATUSRESP," + myName + "," + tokens[1] + ",";
        messegesToBeSentMutex.lock();
        for(auto i : messagesToBeSent){
            if(i.second.size() > 0){
                msg += i.first + "," + std::to_string(i.second.size());
            }
        }
        messegesToBeSentMutex.unlock();
        msg += "04";
        send(serverSocket, msg.c_str(), msg.length(), 0);
    }
    else if(tokens[0].compare("STATUSRESP") == 0){
        std::string msg;
        for(int i = 3; i < tokens.size(); i = i+2){

            msg += tokens[i] + "," + tokens[i+1];
        }
        if(msg.length() == 0){
            std::cout << "server has no messages" << std::endl;
        }
        else{
            std::cout << "Response: " << msg << std::endl;
        }
    }
    else{
        std::cout << "command not found " << std::endl;
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
    if(listen(listenServerSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %s\n", serverPort);
        exit(0);
    }
    else if(serversByGroupId.size() > 4){
        //check a better way to do this
        close(listenServerSock);
    }
    else
        // Add listen socket to socket set we are monitoring
    {
        FD_SET(listenServerSock, &openSockets);
        if(listenServerSock > *maxfds){
            *maxfds = listenServerSock;
        }
    }
    finished = false;
    while(!finished) {
        // Get modifiable copy of readSockets
        readSockets = exceptSockets = openSockets;
        memset(buffer, 0, sizeof(buffer));
        // Look at sockets and see which ones have something to be read()
        int n = -1;
        n = select(*maxfds + 1, &readSockets, NULL, &exceptSockets, NULL);
        if (n < 0) {
            perror("select failed - closing down\n");
            finished = true;
        } else {
            // First, accept  any new connections to the server on the listening socket
            if (FD_ISSET(listenServerSock,&readSockets))                              // Tests to se if listenSock is part of readSockets
            {
                serverSock = accept(listenServerSock, (struct sockaddr *) &server,
                                    &serverLen);                                        // Extracts the first connection request on the queue of pending connections
                printf("accept***\n");                                          // returns a new fd referring to that socket
                // Add new client to the list of open sockets
                FD_SET(serverSock, &openSockets);

                // And update the maximum file descriptor
                *maxfds = std::max(*maxfds, serverSock);

                // create a new server to store information.
                serverMutex.lock();
                servers[serverSock] = new Server(serverSock);
                serverMutex.unlock();
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
                            serverMutex.lock();
                            closeServer(tmpServer->sock, maxfds);
                            serverMutex.unlock();

                        }
                            // We don't check for -1 (nothing received) because select()
                            // only triggers if there is something on the socket for us.
                        else
                        {
                            if(strlen(buffer) != 0){
                                serverCommand(tmpServer->sock, maxfds,
                                              buffer);
                            }
                        }
                    }
                }
                if(serverSock > *maxfds){
                    *maxfds = serverSock;
                }
                struct timeval tv2 = {1, 0};   // sleep for 1 sec!
                int timeout = select(0, NULL, NULL, NULL, &tv2);
                serverMutex.lock();
                bool check = false;
                if(servers.find(serverSock) != servers.end()){
                    if(servers[serverSock]->name == ""){
                        check = true;
                        serverMutex.unlock();
                        handleListServer(serverSock, 0, true, maxfds);
                    }
                }
                if(!check){
                    serverMutex.unlock();
                }
            }
        }
    }

}
void handleClients(int listenClientSock, int clientPort, int *maxfds)
{
    bool finished;
    fd_set openClientSockets;             // Current open sockets
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
        FD_ZERO(&openClientSockets);
        FD_SET(listenClientSock, &openClientSockets);
        *maxfds = listenClientSock;
    }

    finished = false;

    while(!finished)
    {
        // Get modifiable copy of readSockets
        readSockets = exceptSockets = openClientSockets;
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
                FD_SET(clientSock, &openClientSockets);

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

                            closeClient(client->sock, &openClientSockets, maxfds, clients);

                        }
                            // We don't check for -1 (nothing received) because select()
                            // only triggers if there is something on the socket for us.
                        else
                        {
                            clientCommand(client->sock, &openClientSockets, maxfds,
                                          buffer);
                        }
                    }
                }
            }
        }
    }



}
void localServerCommand(const char* buffer, int serverPort, int *maxfds)
{
    std::vector<std::string> tokens;
    std::string token;

    // Split command into tokens for parsing
    std::stringstream stream(buffer);

    while(stream >> token)
        tokens.push_back(token);

    if((tokens[0].compare("01") != 0) || (tokens[tokens.size()-1].compare("04")) != 0)
    {
        printf("Invalid command format, <01><Command>,<comma separated parameters><04>\n");
        return;
    }
    else if((tokens[1].compare("LEAVE") == 0))
    {
        // for now lets just have everything space separated
        // just so we can get the functionality working
        if(tokens.size() != 5){
            printf("Invalid format of LEAVE <01>LEAVE,IP,Port<04>");
            return;
        }
        std::string ip = tokens[2];
        int port = stoi(tokens[3]);

        leaveMapMutex.lock();
        int sockToDisconnect = leaveMap.find(std::make_pair(ip,port))->second;
        leaveMapMutex.unlock();
        if(FD_ISSET(sockToDisconnect, &openSockets))
        {
            int closeVal = close(sockToDisconnect);
            if(closeVal != -1){
                closeServer(sockToDisconnect, maxfds);
            }
        }
        else{
            std::cout << "socket " << sockToDisconnect << " not found" << std::endl;
        }
        leaveMapMutex.lock();
        for(auto const& p : leaveMap){
            std::cout << "p.first.first " << p.first.first <<  std::endl;
            std::cout << "p.first.second " << p.first.second <<  std::endl;
            std::cout << "p.second " << p.second <<  std::endl << std::endl;
        }
        leaveMapMutex.unlock();
    }
    else if((tokens[1].compare("CONNECTLOCAL") == 0))
    {
        std::cout << "connection to local port " << tokens[2] << std::endl;
        handleConnection("127.0.0.1", tokens[2].c_str(), serverPort, maxfds);
    }
    else if((tokens[1].compare("LOCALSERVERS") == 0))
    {
        std::cout << "" << std::endl;
        std::cout << "CURRENT SERVERS" << std::endl;
        std::cout << "serversByGroupID" << std::endl;
        serversByGroupIdMutex.lock();
        for(auto const& p : serversByGroupId){
            std::cout << "p.first: " << p.first << std::endl;
            std::cout << "p.second.first: " << p.second.first << std::endl;
            std::cout << "p.second.second: " << p.second.second << std::endl;
            std::cout << "" << std::endl;
        }
        serversByGroupIdMutex.unlock();
        std::cout << "" << std::endl;
        std::cout << "CURRENT SERVERS" << std::endl;
        std::cout << "leaveMap" << std::endl;
        leaveMapMutex.lock();
        for(auto const& p : leaveMap){
            std::cout << "p.first.first: " << p.first.first << std::endl;
            std::cout << "p.first.second: " << std::to_string(p.first.second) << std::endl;
            std::cout << "p.second: " << std::to_string(p.second) << std::endl;
            std::cout << "" << std::endl;
        }
        leaveMapMutex.unlock();
        std::cout << "size of servers: " << servers.size() << std::endl << std::endl;
    }
    else if(tokens[1].compare("STATUSREQ") == 0){
        serversSocketsMutex.lock();
        auto pos = serversSockets.find(tokens[2]);
        if(pos != serversSockets.end()){
            int sock = pos->second;
            std::string tmp = "01STATUSREQ," + myName + "04";
            send(sock, tmp.c_str(), tmp.length(), 0);
        }
        else{
            std::cout << "group name not found" << std::endl;
        }
        serversSocketsMutex.unlock();
    }
    else{
        std::cout << "no command found, TOKEN[1]: " << tokens[1] << std::endl;
        std::cout << "no command found, TOKEN[2]: " << tokens[2] << std::endl;
    }

}
void handleListServer(int socket, int listenServersPort, bool incomingConnection, int *maxfds)
{
    int nwrite;
    int nread;
    char buffer[1025];
    bzero(buffer, sizeof(buffer));
    strcpy(buffer, "01SERVERS04");
    nwrite = send(socket, buffer, strlen(buffer), 0);
    memset(buffer, 0, sizeof(buffer));
    nread = read(socket, buffer, sizeof(buffer));
    std::stringstream stream(buffer);
    std::string tmp = buffer;
    std::stack<std::string> s;
    bool firstFound = false;
    std::vector<std::string> firstSplit = split(tmp,';');
    for(auto i : firstSplit)
    {
        std::vector<std::string> temp = split(i,',');
        if(!firstFound){
            int port = stoi(temp[2]);
            serversByGroupIdMutex.lock();
            serversByGroupId.insert(std::make_pair(temp[0], std::make_pair(temp[1], port)));
            serversByGroupIdMutex.unlock();
            serversSocketsMutex.lock();
            serversSockets.insert(std::make_pair(temp[0], socket));
            serversSocketsMutex.unlock();
            leaveMapMutex.lock();
            leaveMap.insert(std::make_pair(std::make_pair(temp[1], port), socket));
            leaveMapMutex.unlock();
            serverMutex.lock();
            if(servers.find(socket) == servers.end()){
                servers[socket] = new Server(socket);
            }
            servers.find(socket)->second->name = temp[0];
            servers[socket]->ip = temp[1];
            servers[socket]->port = port;

            serverMutex.unlock();
            firstFound = true;

            if(incomingConnection){
                //if incoming only check the first servers info
                break;
            }
        }
        else {
            //should not be needed since break; above
            if (!incomingConnection) {
                bool check = false;
                serversByGroupIdMutex.lock();
                if(serversByGroupId.find(temp[0]) == serversByGroupId.end()){
                    serversByGroupIdMutex.unlock();
                    check = true;
                    handleConnection(temp[1].c_str(), temp[2].c_str(), listenServersPort, maxfds);
                }
                if(!check){
                    serversByGroupIdMutex.unlock();
                }
            }
        }
    }
}
void handleConnection(const char* ipAddress, const char* port, int listenServersPort, int *maxfds)
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
        return;
    }

    if(connect(tmpSocket, (struct sockaddr*)&serv_Addr, sizeof(serv_Addr)) < 0)
    {
        std::cout << "failed to connect to server at: " << ntohs(serv_Addr.sin_addr.s_addr) << std::endl;
        perror("could not connect ");
        //exit(0);
        return;
    }
    FD_SET(tmpSocket, &openSockets);
    if(tmpSocket > *maxfds){
        *maxfds = tmpSocket;
    }

    handleListServer(tmpSocket, listenServersPort, false, maxfds);
}
void handleServerKeepAlive()
{
    bool finished = false;
    while(!finished)
    {
        struct timeval tv = {60, 0};   // sleep for ten minutes!
        int timeout = select(0, NULL, NULL, NULL, &tv);
        for(auto &server : servers){
            int socket = server.second->sock;
            messegesToBeSentMutex.lock();
            auto pos = messagesToBeSent.find(server.second->name);
            std::string msg;
            if(pos != messagesToBeSent.end()){
                int msgNum = pos->second.size();
                msg = "01 KEEPALIVE, " + std::to_string(msgNum) + " 04";
            }
            else{
                msg = "01 KEEPALIVE, 0 04";
            }
            messegesToBeSentMutex.unlock();
            send(socket, msg.c_str(), strlen(msg.c_str()), 0);
        }
    }
}
void scanForDisconnectedServers(int *maxfds)
{
    bool finished = false;
    while(!finished) {
        struct timeval tv = {120, 0};   // sleep for ten minutes!
        int timeout = select(0, NULL, NULL, NULL, &tv);
        for (auto &server : servers) {
            serverMutex.lock();
            if (!server.second->checkedIn) {
                int closeVal = close(server.second->sock);
                if (closeVal != -1) {
                    closeServer(server.second->sock, maxfds);
                }
            }
            server.second->checkedIn = false;
            serverMutex.unlock();
        }
    }

}
int main(int argc, char* argv[])
{
    if(argc != 6)
    {
        printf("Usage: chat_server <groupname serverPort destIp destPort clientPort>\n");
        exit(0);
    }
    /*
     * Todo 2. after you get a connection send the LISTSERVERS,<FROM GROUP ID>  command to the server
     * Todo 3. begin with inserting the info of the first parameter info the networkInfo (the server that you connected to)
     * Todo 4. after that try to connect to the rest of the servers that he is connected to,
     * Todo remember to make a check for the group_ID so you dont try to make a connection twice
     * Todo 5. when a server successfully connects to your server do the do step 2 and 3 again
     */
    int listenClientSock;                   // Socket for connections to server
    int listenServerSock;                   // Socket for server connections
    int serverPort = atoi(argv[2]);
    int clientPort = atoi(argv[5]);
    char buffer[1025];
    //fd_set openSockets;
    int maxfds;                     // Passed to select() as max fd in set
    std::map<std::string, std::string> networkInfo;
    findMyIp(networkInfo);
    myName = argv[1];
    FD_ZERO(&openSockets);
    listenServerSock = open_socket(atoi(argv[2]));                     // Open the socket for the server connections


    std::map<std::string, std::string>::const_iterator pos = networkInfo.find("eth1");
    std::string groupName(argv[1]);

    currentServer->name = argv[1];
    currentServer->ip = pos->second;
    currentServer->port = atoi(argv[2]);

    /*
    if(pos != networkInfo.end()){
        serversByGroupId.insert(std::make_pair(groupName, std::make_pair(pos->second, serverPort)));
    }
    else{
        printf("eth1 not found\n");
    }
    */
    handleConnection(argv[3], argv[4], serverPort, &maxfds);
    // Setup socket for server to listen to
    //handleConnection("127.0.0.1", argv[4], serverPort, &maxfds);
    listenClientSock = open_socket(clientPort);                     // Open the socket for the client connections


    std::thread clientThread(handleClients, listenClientSock, clientPort, &maxfds);
    std::thread serverThread(handleServers,listenServerSock, serverPort, &maxfds);
    std::thread keepAliveThread(handleServerKeepAlive);
    std::thread scanDisconnectedThread(scanForDisconnectedServers, &maxfds);
    //handleServers(listenServerSock, serverPort, servers, serversByGroupId);
    //handleConnection(argv[3], argv[4], serverPort); //HERE AFTER THE TEST


    bool finished = false;

    while(!finished){
        bzero(buffer, sizeof(buffer));
        fgets(buffer, sizeof(buffer), stdin);
        localServerCommand(buffer, serverPort, &maxfds);
    }
    serverThread.join();
    clientThread.join();

    return 0;
}
