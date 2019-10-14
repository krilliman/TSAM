//
// Simple server for botnet activity
//
//
// Author: Kristmann ingi Kristjánsson && Helgi Rúnar Jóhannesson
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
    bool checkedIn = false;
    Server(int socket) : sock(socket){}

    ~Server(){}            // Virtual destructor defined for base class
};
//All our global functions.
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

//All our global variables.
fd_set openSockets;
std::map<std::string, int> serversSockets;                              // Lookup table to get sockets by groupName
std::map<std::string, std::pair<std::string, int>> serversByGroupId;    // Lookup table to get <Ip,Port> by groupName
std::map<std::pair<std::string,int>,int> leaveMap;                      // Lookup to get socket by <Ip,Port>
std::map<int, Client*> clients;                                         // Lookup table for per Client information
std::map<int, Server*> servers;                                         // Lookup table for per Server information
std::string myName;                                                     // This servers name
std::map<std::string, std::vector<std::string>> serverMessages;         // Lookup table for messages received from other servers
std::map<std::string, std::vector<std::string>> messagesToBeSent;       // Lookup table for messages that client tried to send to but were not 1-hop away

Server *currentServer = new Server(0);

// mutex for maps
std::mutex serverMutex;             // Mutex for the servers Map
std::mutex serversSocketsMutex;     // Mutex for serversSocket Map
std::mutex serversByGroupIdMutex;   // Mutex for serversByGroupId Map
std::mutex leaveMapMutex;           // Mutex for leaveMap
std::mutex serverMessagesMutex;     // Mutex for serverMessages Map
std::mutex messegesToBeSentMutex;   // Mutex for messegesToBeSent Map


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
    if(servers.find(serverSocket) == servers.end()){
        std::cout << "server not found " << std::endl;
    }
    std::string groupName = servers.find(serverSocket)->second->name;
    auto pp = std::make_pair(servers.find(serverSocket)->second->ip, servers.find(serverSocket)->second->port);

    /*
     * Here we the leaving socket from all our maps
     * node that we also use mutex to lock each data structure so we dont get unexpected behavior
     */
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

    // check if the socket leaveing is the current max file descriptor we need to update our maxfds

    if(*maxfds == serverSocket)
    {
        for(auto const& p : servers)
        {
            *maxfds = std::max(*maxfds, p.second->sock);
        }
    }
    // And remove from the list of open sockets.
    FD_CLR(serverSocket, &openSockets);
}
// Process command from clients on the server
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
            //locking the data structure
            serverMessagesMutex.lock();
            auto pos = serverMessages.find(myName);//Iterates through the map to find if there's already message from that server.
            if(pos != serverMessages.end())
            {
                pos->second.push_back(msg);//Pushes into the vector if there is.
            }
            else
            {
                std::vector<std::string> tmpVector;
                tmpVector.push_back(msg);
                serverMessages.insert(std::make_pair(myName, tmpVector));// Otherwie inserts the name and message into the map.
            }
            serverMessagesMutex.unlock();
        }
        else//If it's not meant for us, we send it forward.
        {
            sendMSG(tokens[1], msg.c_str());
        }
    }
    else if(tokens[0].compare("GETMSG") == 0)//Gets a message.
    {
        getMSG(clientSocket, tokens[1]);
    }
    else if(tokens[0].compare("LISTSERVERS") == 0)//List the servers
    {
        serverList(clientSocket,myName,buffer);
    }
    else
    {
        std::cout << "Unknown command from client:" << buffer << std::endl;
    }

}
//Processes our server list into a string and then sends it to the socket that requested it.
void serverList(int socket, std::string groupName, char *buffer)
{
    std::string msg = "\1SERVERS," + currentServer->name + "," + currentServer->ip + "," + std::to_string(currentServer->port) + ";";
    serversByGroupIdMutex.lock();
    for(auto const & p : serversByGroupId){
        msg += p.first + "," + p.second.first + "," + std::to_string(p.second.second) + ";";
    }
    serversByGroupIdMutex.unlock();
    msg += "\4";
    send(socket, msg.c_str(), msg.length(), 0);
}
//Split the command string into a string vector.
std::vector<std::string> split(const std::string& s, char delimiter)
{
    std::string text = s;
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
// getMSG. Called when server calls getmsg to us and we respons with
// a message intended for them and then delete that part of the map.
void getMSG(int socket, std::string groupName)
{
    std::string msg;
    // lock the data structure
    serverMessagesMutex.lock();
    auto pos = serverMessages.find(groupName);
    if(pos != serverMessages.end())
    {
        // pop one message from the vector
        msg = pos->second.front();
        pos->second.erase(pos->second.begin());
        // send the message
        send(socket, msg.c_str(),msg.length(),0);
        serverMessagesMutex.unlock();
        return;
    }
    serverMessagesMutex.unlock();
    msg = "I don't have any messages from this server.";
    send(socket, msg.c_str(),msg.length(),0);
}
//Called when a server we're not connected to requrests messages from us.
void emptyMessagesToBeSent(std::string groupName, int serverSocket)
{
    // lock the data structure
    messegesToBeSentMutex.lock();
    auto pos = messagesToBeSent.find(groupName);
    // enter if the group name is not found
    if(pos == messagesToBeSent.end()){
        std::string msg = "\1SEND_MSG," + myName + "," + groupName + ",No messages found\4";
        send(serverSocket, msg.c_str(), msg.length(), 0);
        messegesToBeSentMutex.unlock();
        return;
    }
    // if the group name is found we loop over each server in the messageTobeSent
    // and for each of the messages in the vector we send them over to the server
    for(auto s : pos->second){
        sendMSG(groupName, s.c_str());
        struct timeval tv = {1, 0};   // sleep for 1 second.
        int timeout = select(0, NULL, NULL, NULL, &tv);
    }
    // delete the groupName from the data structure since the vector is should be empty
    messagesToBeSent.erase(groupName);
    messegesToBeSentMutex.unlock();
}
//Called when client calls sendmsg and we send it forward to the next server.
void sendMSG(std::string groupName, const char *msg)
{
    // lock the data structure
    serversSocketsMutex.lock();
    auto pos = serversSockets.find(groupName);
    // enter if groupName is not found in serversSockets
    if(pos == serversSockets.end()){
        messegesToBeSentMutex.lock();
        auto toBeSentPos = messagesToBeSent.find(groupName);
        // if groupName is not found in messageTo be sent
        // then we create a vector and push the msg into the vector
        // then we make a pair of the groupName and the vector created
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
    // if the groupName is found we send the message to the server
    serversSocketsMutex.unlock();
    int socket = pos->second;
    std::string buffer = "\1SEND_MSG," + myName + "," + groupName + "," + msg + "\4";
    int bSent = send(socket, buffer.c_str(), strlen(buffer.c_str()), 0);
}
//Processes commands that are sent to the server and sent by the server.
void serverCommand(int serverSocket, int *maxfds, char *buffer)
{
    std::string text = buffer;
    text = text.substr(1, text.length()-2);
    std::vector<std::string> tmp = split(text,';');
    std::vector<std::string> tokens;
    for(auto i : tmp)
    {
        tokens = split(i,',');
    }
    if((tokens[0].compare("SERVERS")) == 0){
        serverList(serverSocket,myName,buffer);
    }
    else if((tokens[0].compare("LISTSERVERS")) == 0){
        serverList(serverSocket,tokens[1],buffer);
    }
    else if((tokens[0].compare("SEND_MSG")) == 0){
        serverMessagesMutex.lock();
        auto pos = serverMessages.find(tokens[1]);
        // if we have not received messages for this group we create a vector
        // push back the message from the server
        // then make a pair of the groupName and the vector created
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
            printf("Invalid command format, format: <KEEPALIVE>,<No, messages>\n");
            return;
        }
        // set this to true so that our scanner knows that this server has checked in
        servers[serverSocket]->checkedIn = true;
        int msg = stoi(tokens[1]);
        if(msg > 0){
            std::string getmsg = "\1GET_MSG," + myName + "\4";
            send(serverSocket, getmsg.c_str(), getmsg.length(), 0);
        }
    }
    else if(tokens[0].compare("GET_MSG") == 0){
        if(tokens.size() != 2){
            printf("Invalid command format, format: <GETMSG>,<GROUP ID>\n");
            return;
        }
        emptyMessagesToBeSent(tokens[1], serverSocket);
    }
    else if(tokens[0].compare("STATUSREQ") == 0){
        if(tokens.size() != 2){
            printf("Invalid command format, format: <STATUSREQ>,<FROM GROUP>\n");
            return;
        }
        std::string msg = "\1STATUSRESP," + myName + "," + tokens[1] + ",";
        // loop over all messages that we have stored for servers that were not 1 hop a way
        // when client sent the message and create a string to send to the server
        messegesToBeSentMutex.lock();
        for(auto i : messagesToBeSent){
            if(i.second.size() > 0){
                msg += i.first + "," + std::to_string(i.second.size());
            }
        }
        messegesToBeSentMutex.unlock();
        msg += "\4";
        send(serverSocket, msg.c_str(), msg.length(), 0);
    }
    else if(tokens[0].compare("STATUSRESP") == 0){
        // take the respones form the statusREQ and print it out
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
//Handles the servers that are connected to our server.
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
                FD_SET(serverSock, &openSockets);                               // add the serversSock into open sockets

                // if the serverSock is bigger the the max file descriptor then we update maxfds
                if(serverSock > *maxfds){
                    *maxfds = serverSock;
                }

                // call this function so we can know the users info
                handleListServer(serverSock, listenServerSock, true, maxfds);


                // And update the maximum file descriptor
                *maxfds = std::max(*maxfds, serverSock);

                // create a new server to store information.
                serverMutex.lock();
                servers[serverSock] = new Server(serverSock);
                serverMutex.unlock();
                n--;
                printf("Servers connected on server: %d\n", servers.size());
            }
            while(n-- > 0)
            {
                for(auto const& pair : servers)
                {
                    Server *tmpServer = pair.second;

                    if(FD_ISSET(tmpServer->sock, &readSockets))
                    {
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
            }
        }
    }

}
//Handles the client that's connected to our server.
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
//Local commands used to testing and not connecting to other servers
void localServerCommand(const char* buffer, int serverPort, int *maxfds)
{
    std::vector<std::string> tokens;
    std::string token;
    // Split command into tokens for parsing
    std::stringstream stream(buffer);

    while(stream >> token)
        tokens.push_back(token);

    // In local commands we have a diffrent format from the server/server
    // format: 01 <Command> <Command Values> 04
    if((tokens[0].compare("01") != 0) || (tokens[tokens.size()-1].compare("04")) != 0)
    {
        printf("Invalid command format, <<Command>,<comma separated parameters>\n");
        return;
    }
    else if((tokens[1].compare("LEAVE") == 0))
    {
        // Local leave command with te format 01 <Leave> <ip>,<Port> 04
        if(tokens.size() != 5){
            printf("Invalid format of LEAVE IP,Port");
            return;
        }
        std::string ip = tokens[2];
        int port = stoi(tokens[3]);

        // lock the leaveMap data structure then disconnect the socket
        // found with the following <ip><port> pair.
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
    }
    else if((tokens[1].compare("CONNECTLOCAL") == 0))
    {
        // connect to a local port
        // mainly for testing server
        std::cout << "connection to local port " << tokens[2] << std::endl;
        handleConnection("127.0.0.1", tokens[2].c_str(), serverPort, maxfds);
    }
    else if((tokens[1].compare("LOCALSERVERS") == 0))
    {
        // this command is to see all the local connected servers

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
        std::cout << "size of servers: " << servers.size() << std::endl << std::endl;
    }
    else if(tokens[1].compare("STATUSREQ") == 0){
        // use this command to send a STATUSREQ to another server
        // format: 01 STATUSREQ <GROUP_NAME> 04
        serversSocketsMutex.lock();
        auto pos = serversSockets.find(tokens[2]);
        if(pos != serversSockets.end()){
            int sock = pos->second;
            std::string tmp = "\1STATUSREQ," + myName + "\4";
            send(sock, tmp.c_str(), tmp.length(), 0);
        }
        else{
            std::cout << "group name not found" << std::endl;
        }
        serversSocketsMutex.unlock();
    }
    else{
        std::cout << "no command found"  << std::endl;
    }

}
//Handles the servers that we get when we ask for the LISTSERVERS to the other server.
void handleListServer(int socket, int listenServersPort, bool incomingConnection, int *maxfds)
{
    int nwrite;
    int nread;
    char buffer[1025];
    bzero(buffer, sizeof(buffer));
    memset(buffer, 0, sizeof(buffer));
    std::string sendVal = "\1LISTSERVERS," + myName + "\4";
    //send LISTSERVER command to the server that just connected
    nwrite = send(socket, sendVal.c_str(), sendVal.length(), 0);
    nread = read(socket, buffer, sizeof(buffer));
    if(nread == -1){
        perror("no response from server");
        return;
    }
    std::cout << "in handlelist buffer: " << buffer << std::endl;
    std::cout << "nread: " << nread << std::endl;
    std::string tmp = buffer;
    tmp = tmp.substr(9,tmp.length()-11);
    bool firstFound = false;
    std::cout << "tmp: " << tmp << std::endl;
    // split the buffer frist by ;
    // then we split it by ,
    // that should give us 3 elements 1:name, 2:ip, 3:port
    std::vector<std::string> firstSplit = split(tmp,';');
    for(auto i : firstSplit)
    {
        std::cout << "i: " << i << std::endl;
        std::vector<std::string> temp = split(i,',');
        if(temp[0] == myName){
            continue;
        }
        if(!firstFound){
            // if firstFound is false then the data in temp is the info about the server that just connected to us
            // then we insert the data into all our data structures
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
                // this triggers if we are making the inital connection
                // but not if we are the server that is being connected to
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
                // since we use this function both for incoming connections and for connecting to other servers
                // we need to check if this is coming from the initial connection
                // if this is coming from the initial connection we recursively try to connect to all other servers
                // that the newly connected server is connected to
                bool check = false;
                serversByGroupIdMutex.lock();
                if(serversByGroupId.find(temp[0]) == serversByGroupId.end()){
                    serversByGroupIdMutex.unlock();
                    check = true;
                    handleConnection(temp[1].c_str(), temp[2].c_str(), listenServersPort, maxfds);
                }
                if(!check){
                    // we where getting strange behavior when we unlocked on a unlocked mutex so had to check if the check above occured
                    serversByGroupIdMutex.unlock();
                }
            }
        }
    }
}
void handleConnection(const char* ipAddress, const char* port, int listenServersPort, int *maxfds)
{
    // if our map is bigger then 4 we stop the recursion that is being called from handleListServer
    if(serversByGroupId.size() > 4){
        return;
    }
    struct sockaddr_in serv_Addr;
    char buffer[1025];
    struct sockaddr_in sk_addr;   // address settings for bind()
    struct hostent *server;
    int tmpSocket;
    int set = 1;
    server = gethostbyname(ipAddress);
    bzero((char*) &serv_Addr, sizeof(serv_Addr));
    serv_Addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serv_Addr.sin_addr.s_addr, server->h_length);
    serv_Addr.sin_port = htons(atoi(port));

    struct timeval tv = {10, 0};


    tmpSocket = socket(AF_INET, SOCK_STREAM , 0);
    if(setsockopt(tmpSocket, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
    {
        perror("setsockopt failed: ");
    }
    // set a timeout on the socket for 5 seconds
    if(setsockopt(tmpSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv)) < 0)
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
        perror("could not connect ");
        //exit(0);
        return;
    }
    FD_SET(tmpSocket, &openSockets);
    if(tmpSocket > *maxfds){
        *maxfds = tmpSocket;
    }
    bzero(buffer, sizeof(buffer));

    // we expect a LISTSERVER from the server that we are connecting to
    // we wait 5 seconds if the time
    int nread = read(tmpSocket, buffer, sizeof(buffer));
    std::cout << "in handleconnection: " << buffer  << std::endl;
    std::cout << "nread in connection: " << nread << std::endl;
    if(nread != -1)
    {
        // if we get a response from the server we process the message
        serverCommand(tmpSocket, maxfds, buffer);
    }
    struct timeval tv2 = {1, 0};   // sleep for 1 sec!
    int timeout = select(0, NULL, NULL, NULL, &tv2);
    std::cout << "entering handleListServer "  << std::endl;
    handleListServer(tmpSocket, listenServersPort, false, maxfds);
}
//function for handling the keepalive for the server.
void handleServerKeepAlive()
{

    //every one mintues we loop over all our connected servers and send the keepalive message
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
                msg = "\1KEEPALIVE, " + std::to_string(msgNum) + "\4";
            }
            else{
                msg = "\1KEEPALIVE, 0\4";
            }
            messegesToBeSentMutex.unlock();
            send(socket, msg.c_str(), strlen(msg.c_str()), 0);
        }
    }
}

void scanForDisconnectedServers(int *maxfds)
{
    /*
     * every 2 minutes or so wee loop over all or servers and check if the server has send us a keepalive message the last 2 minutes
     * if this is false we drop the connection to the server
     */
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
            // set this to false so we know if he sends us keepalive message the next 2 minutes
            server.second->checkedIn = false;
            serverMutex.unlock();
        }
    }

}

int main(int argc, char* argv[])
{
    if(argc != 5)
    {
        printf("Usage: chat_server <groupname serverPort destIp destPort clientPort>\n");
        exit(0);
    }
    int listenClientSock;                   // Socket for connections to server
    int listenServerSock;                   // Socket for server connections
    int serverPort = atoi(argv[2]);
    int clientPort = 10000;
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

    //set info for the current server
    currentServer->name = argv[1];
    currentServer->ip = pos->second;
    currentServer->port = atoi(argv[2]);

    handleConnection(argv[3], argv[4], serverPort, &maxfds);
    // Setup socket for server to listen to
    listenClientSock = open_socket(clientPort);                     // Open the socket for the client connections

    //start a thread to handle all our clients
    std::thread clientThread(handleClients, listenClientSock, clientPort, &maxfds);
    //start a thread to handle all our servers
    std::thread serverThread(handleServers,listenServerSock, serverPort, &maxfds);
    // start a thread to handle all our keepAlive messages
    std::thread keepAliveThread(handleServerKeepAlive);
    // start a thread to check if servers have checked in with a keepalive message
    std::thread scanDisconnectedThread(scanForDisconnectedServers, &maxfds);

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
