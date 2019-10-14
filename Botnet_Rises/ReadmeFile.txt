=====Steps=====
1. run the makefile.
2. launch the server with paremeters = ./server <GROUP_NAME> <PORT> <INITIAL_IP> <INITIAL_PORT>
3. launch the client with parameters = ./client 127.0.0.1 10000
4. Using the client to send messages and getting messages from servers via their commands:
    GETMSG,<GROUP_NAME> - Gets a message from the group_name.
    SENDMSG,<GROUP_NAME>,<MESSAGE> - Sends a message to the group_name.
    LISTSERVERS - Prints out all information about the servers that are connected.
5. Using the server, you can call multiple local commands via their commands:
    01 LEAVE <IP> <PORT> 04 - Disconnects from the server with the IP and PORT.
    01 CONNECTLOCAL <PORT> 04 - Connects to the local PORT on your machine.
    01 LOCALSERVERS 04 - Lists alls the servers connected to your servers.
    01 STATUSREQ <GROUP_NAME> 04 - 

