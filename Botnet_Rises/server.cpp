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
void closeServer(int serverSocket, fd_set *openSockets, int *maxfds, std::map<int, Server*> servers)
{
    // Remove client from the clients list
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

    // And remove from the list of open sockets.

    FD_CLR(serverSocket, openSockets);
}
// Process command from client on the server

void clientCommand(int clientSocket, fd_set *openSockets, int *maxfds, 
                  char *buffer, std::map<int, Client*> clients)
{

  std::vector<std::string> tokens;
  std::string token;
  // Split command from client into tokens for parsing
  std::stringstream stream(buffer);

  while(stream >> token)
      tokens.push_back(token);

  if((tokens[0].compare("CONNECT") == 0) && (tokens.size() == 2))
  {
     clients[clientSocket]->name = tokens[1];
  }
  else if(tokens[0].compare("LEAVE") == 0)
  {
      // Close the socket, and leave the socket handling
      // code to deal with tidying up clients etc. when
      // select() detects the OS has torn down the connection.
 
      closeClient(clientSocket, openSockets, maxfds, clients);
  }
  else if(tokens[0].compare("WHO") == 0)
  {
     std::cout << "Who is logged on" << std::endl;
     std::string msg;

     for(auto const& names : clients)
     {
        msg += names.second->name + ",";

     }
     // Reducing the msg length by 1 loses the excess "," - which
     // granted is totally cheating.
     send(clientSocket, msg.c_str(), msg.length()-1, 0);

  }
  // This is slightly fragile, since it's relying on the order
  // of evaluation of the if statement.
  else if((tokens[0].compare("MSG") == 0) && (tokens[1].compare("ALL") == 0))
  {
      std::string msg;
      for(auto i = tokens.begin()+2;i != tokens.end();i++) 
      {
          msg += *i + " ";
      }

      for(auto const& pair : clients)
      {
          send(pair.second->sock, msg.c_str(), msg.length(),0);
      }
  }
  else if(tokens[0].compare("MSG") == 0)
  {
      for(auto const& pair : clients)
      {
          if(pair.second->name.compare(tokens[1]) == 0)
          {
              std::string msg;
              for(auto i = tokens.begin()+2;i != tokens.end();i++) 
              {
                  msg += *i + " ";
              }
              send(pair.second->sock, msg.c_str(), msg.length(),0);
          }
      }
  }
  else
  {
      std::cout << "Unknown command from client:" << buffer << std::endl;
  }
     
}

void serverCommand(int serverSocket, fd_set *openSockets, int *maxfds, char *buffer, std::map<int, Server*> servers, std::map<std::string, std::pair<std::string, int>> serversByGroupId)
{
    std::vector<std::string> tokens;
    std::string token;

    // Split command from client into tokens for parsing
    std::stringstream stream(buffer);

    while(stream >> token)
        tokens.push_back(token);

    if((tokens[0].compare("01") != 0) || (tokens[tokens.size()-1].compare("04")) != 0)
    {
        printf("Invalid command format, <01><Command>,< comma separated parameters > <04>\n");
        return;
    }
    else if((tokens[1].compare("SERVERS")) == 0){
        std::string str;
        for(auto const & p : serversByGroupId){
            str += p.first + ", " + p.second.first + ", " + std::to_string(p.second.second) + ";";
        }
        send(serverSocket, str.c_str(), str.length(),0);
        std::cout << "str: " << str << std::endl;
    }

}

void handleServers(int listenServerSock, int serverPort, std::map<int ,Server*> servers, std::map<std::string, std::pair<std::string, int>> serversByGroupId, fd_set &openSockets)
{
    bool finished;
    //fd_set openSockets;             // Current open sockets
    fd_set readSockets;             // Socket list for select()
    fd_set exceptSockets;           // Exception socket list
    int maxfds;                     // Passed to select() as max fd in set
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
        FD_ZERO(&openSockets);
        FD_SET(listenServerSock, &openSockets);
        maxfds = listenServerSock;
    }
    finished = false;

    while(!finished) {
        // Get modifiable copy of readSockets
        readSockets = exceptSockets = openSockets;
        memset(buffer, 0, sizeof(buffer));
        // Look at sockets and see which ones have something to be read()
        int n = select(maxfds + 1, &readSockets, NULL, &exceptSockets, NULL);

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
                maxfds = std::max(maxfds, serverSock);

                // create a new client to store information.
                servers[serverSock] = new Server(serverSock);

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

                            closeServer(tmpServer->sock, &openSockets, &maxfds, servers);

                        }
                            // We don't check for -1 (nothing received) because select()
                            // only triggers if there is something on the socket for us.
                        else
                        {
                            //std::cout << buffer << std::endl;
                            serverCommand(tmpServer->sock, &openSockets, &maxfds,
                                         buffer, servers, serversByGroupId);
                        }
                    }
                }
            }
        }
    }

}
void handleClients(int listenClientSock, int clientPort, std::map<int, Client*> clients)
{
    bool finished;
    fd_set openSockets;             // Current open sockets
    fd_set readSockets;             // Socket list for select()
    fd_set exceptSockets;           // Exception socket list
    int maxfds;                     // Passed to select() as max fd in set
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
        maxfds = listenClientSock;
    }

    finished = false;

    while(!finished)
    {
        // Get modifiable copy of readSockets
        readSockets = exceptSockets = openSockets;
        memset(buffer, 0, sizeof(buffer));

        // Look at sockets and see which ones have something to be read()
        int n = select(maxfds + 1, &readSockets, NULL, &exceptSockets, NULL);

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
                maxfds = std::max(maxfds, clientSock) ;

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

                            closeClient(client->sock, &openSockets, &maxfds, clients);

                        }
                            // We don't check for -1 (nothing received) because select()
                            // only triggers if there is something on the socket for us.
                        else
                        {
                            time_t rawtime;
                            struct tm * timeinfo;

                            time (&rawtime);
                            timeinfo = localtime (&rawtime);
                            char *data = asctime(timeinfo);
                            data[strlen(data)-1] = '\0';
                            std::cout << "["<<  data << "]:"<< buffer << std::endl;
                            clientCommand(client->sock, &openSockets, &maxfds,
                                          buffer, clients);
                        }
                    }
                }
            }
        }
    }



}

void handleFirstConnection(const char* ipAddress, const char* port, int listenServersPort, fd_set &openSockets, std::map<std::string, int> &serversSockets, std::map<std::string, std::pair<std::string, int>> &serversByGroupId)
{
    struct sockaddr_in serv_Addr;
    struct sockaddr_in sk_addr;   // address settings for bind()
    struct hostent *server;
    int tmpSocket;
    int set = 1;
    char buffer[1025];
    int nwrite;
    int nread;

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
        exit(0);
    }

    if(connect(tmpSocket, (struct sockaddr*)&serv_Addr, sizeof(serv_Addr)) < 0)
    {
        std::cout << "failed to connect to server at: " << ntohs(serv_Addr.sin_addr.s_addr) << std::endl;
        perror("could not connect ");
        exit(0);
    }

    bzero(buffer, sizeof(buffer));
    strcpy(buffer, "01 SERVERS 04");
    nwrite = send(tmpSocket, buffer, strlen(buffer), 0);
    memset(buffer, 0, sizeof(buffer));
    nread = read(tmpSocket, buffer, sizeof(buffer));

    std::stringstream stream(buffer);
    std::string tmp;
    std::stack<std::string> s;
    bool firstFound = false;
    while(stream >> tmp){
        char lastOne = tmp[tmp.length()-1];
        std::string newVal = tmp.substr(0,tmp.length()-1)
        if(lastOne == ';'){
            std::string ip = s.top();
            s.pop();
            std::string groupName = s.top();
            s.pop();
            if(!firstFound){
                serversByGroupId.insert(std::make_pair(groupName, std::make_pair(ip, stoi(newVal))));
                serversSockets.insert(std::make_pair(groupName, stoi(newVal)));
                firstFound = true;
            }
            /*
             * ToDo need to save the following connections and try to make a connection to them
             */
        }
        else{
            s.push(newVal);
        }
    }

    /*
     * ToDo need to process the message and insert to the std::map<std::string, int> serversSockets
     * ToDo the right values
     */
    FD_SET(tmpSocket, &openSockets);
}

int main(int argc, char* argv[])
{
    if(argc != 4)
    {
        printf("Usage: chat_server <port destIp destPort>\n");
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
    int serverPort = atoi(argv[1]);
    int clientPort = 10000;
    fd_set openSockets;
    std::map<std::string, std::string> networkInfo;
    std::map<std::string, int> serversSockets;
    std::map<std::string, std::pair<std::string, int>> serversByGroupId;
    findMyIp(networkInfo);

    listenServerSock = open_socket(serverPort);                     // Open the socket for the server connections

    handleFirstConnection(argv[2], argv[3], serverPort, openSockets, serversSockets, serversByGroupId);

    std::map<int, Client*> clients;         // Lookup table for per Client information
    std::map<int, Server*> servers;         // Lookup table for per Server information

    std::map<std::string, std::string>::const_iterator pos = networkInfo.find("eth1");
    if(pos != networkInfo.end()){
        serversByGroupId.insert(std::make_pair("P3_GROUP65", std::make_pair(pos->second, serverPort)));
    }
    else{
        printf("eth1 not found\n");
    }

    // Setup socket for server to listen to

    listenClientSock = open_socket(clientPort);                     // Open the socket for the client connections


    for(auto const& p : serversByGroupId){
        std::cout << "p first " << p.first << "  p.second.first " << p.second.first << "  p.second.second " << p.second.second << std::endl;
    }

    std::thread clientThread(handleClients, listenClientSock, clientPort, clients);
    //std::thread serverThread(handleServers,listenServerSock, serverPort, servers, serversByGroupId, openSockets);
    handleServers(listenServerSock, serverPort, servers, serversByGroupId, openSockets);

    /*
     * ToDo Here should come a while loop for the handling the messages
     * ToDo to other servers.
     */

    //serverThread.join();
    //clientThread.join();

    return 0;
}
