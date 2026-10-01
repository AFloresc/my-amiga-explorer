/*
 * AEClientThread.c
 *
 *  Created on: May 16, 2021
 *      Author: rony
 */

#define DBGOUT 0

#ifdef __GNUC__
#include <stdio.h>
#include <unistd.h>
#include <sys/filio.h>
#include <netinet/tcp.h>
#include <proto/dos.h>
#include <clib/exec_protos.h>
//#include <clib/timer_protos.h>
#include <inline/timer.h>
#include <dos/dostags.h>
#define __BSDSOCKET_NOLIBBASE__
#include <proto/bsdsocket.h>
#endif

#ifdef __VBCC__

#endif

#include "AEClientThread.h"
#include "AETypes.h"
#include "AEUtil.h"
#include "DirectoryList.h"
#include "SendFile.h"
#include "ReceiveFile.h"
#include "MakeDir.h"
#include "VolumeList.h"
#include "DeletePath.h"


extern char g_KeepServerRunning;
extern volatile LONG g_ActiveThreadCount;

static unsigned short g_NextClientPort = MAIN_LISTEN_PORTNUMBER + 1;

static void clientThread();
static void clientThreadBody();

ClientThread_t *g_ClientThreadList;
struct SignalSemaphore g_ClientThreadListLock;

void initialiseClientThreadList()
{
    //Initialise and then obtain the semaphore, so we won't be disturbed
    InitSemaphore( &g_ClientThreadListLock );
    lockClientThreadList();

    //Initialise the list.  This is a doubly linked list with NULL objects at each end
    ClientThread_t *head = AllocVec( sizeof( ClientThread_t ), MEMF_CLEAR|MEMF_FAST );
    ClientThread_t *tail = AllocVec( sizeof( ClientThread_t ), MEMF_CLEAR|MEMF_FAST );
    head->next = tail;
    tail->previous = head;
    g_ClientThreadList = head;

    //We are done
    unlockClientThreadList();
}

void freeClientThreadList( ClientThread_t *list )
{
    ClientThread_t *head = list;
    ClientThread_t *node = head->next;
    while( node->next )
    {
        ClientThread_t *nextNode = node->next;
        FreeVec( node );
        node = nextNode;
    }
    FreeVec( node );    //This should be the tail
    FreeVec( head );
}

//Free the global client list itself (the head/tail sentinels allocated in
//initialiseClientThreadList).  Call once at shutdown, after all clients are gone.
void destroyClientThreadList()
{
    lockClientThreadList();
    if( g_ClientThreadList != NULL )
    {
        freeClientThreadList( g_ClientThreadList );
        g_ClientThreadList = NULL;
    }
    unlockClientThreadList();
}

void addClientThreadToList( ClientThread_t *client )
{
    //Lock the list
    lockClientThreadList();

    //Add this to the front of the list
    ClientThread_t *head = g_ClientThreadList;
    ClientThread_t *first = head->next;
    client->previous = head;
    client->next = first;
    head->next = client;
    first->previous = client;

    //Unlock the list
    unlockClientThreadList();
}

void removeClientThreadFromList( ClientThread_t *client )
{
    //Lock the list
    lockClientThreadList();

    //Find the entry in question
    ClientThread_t *node = g_ClientThreadList->next;
    while( node->next )
    {
        ClientThread_t *nextNode = node->next;
        ClientThread_t *previousNode = node->previous;
        if( node == client || node->process == client->process )
        {
            //We found the node in question.
            //Relink the nodes before and after this node with each other
            nextNode->previous = previousNode;
            previousNode->next = nextNode;
            client->next = NULL;
            client->previous = NULL;

            //Free the unlinked list entry.  It was AllocVec'd per connection in
            //startClientThread(); without this it leaks on every disconnect and the
            //memory is never reclaimed (AmigaOS does not free a process's
            //allocations on exit).
            FreeVec( node );

            unlockClientThreadList();
            return;
        }

        node = node->next;
    }

    //If we made it here, then we never found the client thread.
    dbglog( "[%s] We didn't find this client thread.\n", __FUNCTION__ );
    unlockClientThreadList();
}

ClientThread_t *getClientThreadList()
{
    //Make a copy and return that
    ClientThread_t *head = AllocVec( sizeof( ClientThread_t ), MEMF_FAST|MEMF_CLEAR );
    ClientThread_t *tail = AllocVec( sizeof( ClientThread_t ), MEMF_FAST|MEMF_CLEAR );
    head->next = tail;
    tail->previous = head;

    ClientThread_t *node = g_ClientThreadList->next;
    while( node->next )
    {
        //Insert the new node
        ClientThread_t *nodeCopy = AllocVec( sizeof( ClientThread_t ), MEMF_FAST|MEMF_CLEAR );
        nodeCopy->next = head->next;
        head->next->previous = nodeCopy;
        nodeCopy->previous = head;
        head->next = nodeCopy;

        //Copy the contents
        memcpy( nodeCopy->ip, node->ip, sizeof( node->ip ) );
        nodeCopy->port = node->port;
        nodeCopy->messagePort = node->messagePort;

        //Next thread
        node = node->next;
    }
    return head;
}

void lockClientThreadList()
{
    ObtainSemaphore( &g_ClientThreadListLock );
}

void unlockClientThreadList()
{
    ReleaseSemaphore( &g_ClientThreadListLock );
}

static void removeClientByPort( UWORD port )
{
    //Traverse the list and find it in the list
    ClientThread_t *node = g_ClientThreadList->next;
    while( node->next )
    {
        if( node->port == port )
        {
            dbglog( "[?] Removed client with port allocation %u from client list.\n", port );
            removeClientThreadFromList( node );
            return;
        }
        node = node->next;
    }
}

UBYTE getClientListSize()
{
    UBYTE count = 0;
    ClientThread_t *node = g_ClientThreadList->next;
    while( node->next ) {   count++; node = node->next; }
    return count;
}

void getIPFromClient( ClientThread_t *client, char ipAddress[ 17 ] )
{
    //We assume that the caller is smart enough to allocate the required bytes in the string
    snprintf( ipAddress, 17, "%u.%u.%u.%u", 
                    (UBYTE)client->ip[0], 
                    (UBYTE)client->ip[1],
                    (UBYTE)client->ip[2],
                    (UBYTE)client->ip[3] );
}

void startClientThread( struct Library *SocketBase, struct MsgPort *msgPort, SOCKET clientSocket )
{
    dbglog( "[master] Accepted socket connection.\n" );
    if ( DOSBase )
    {
        //Start a client thread
        struct TagItem tags[] = {
                { NP_StackSize,     16384 },
                { NP_Name,          (ULONG)"AEClientThread" },
                { NP_Entry,         (ULONG)clientThread },
                { NP_Priority,      (ULONG)8 },
                { NP_Synchronous,   FALSE },
                { TAG_DONE, 0UL }
        };
        dbglog( "[master] Starting client thread.\n" );
        struct Process *clientProcess = CreateNewProc(tags);
        if( clientProcess == NULL )
        {
            dbglog( "[master] Failed to create client thread.\n" );
            CloseSocket( clientSocket );
            return;
        }
        //Count this live child so the master waits for it before unloading the seglist.
        Forbid(); g_ActiveThreadCount++; Permit();

        //Wait (bounded) for the child to tell us its listen port.
        dbglog( "[master] Waiting for child to tell us what port the client should reconnect on.\n" );
        struct Message *clientMessage = NULL;
        struct AEMessage *newClientMessage = NULL;
        LONG regTimeout = 250;  //~5s at Delay(2)
        while( regTimeout-- > 0 )
        {
            clientMessage = GetMsg( msgPort );
            if( clientMessage != NULL )
            {
                newClientMessage = (struct AEMessage *)clientMessage;

                //Shutdown can arrive on this shared port while we are mid-registration.
                if( newClientMessage->messageType == AEM_Shutdown
                 || newClientMessage->messageType == WB_ICON_DOUBLE_CLICKED )
                {
                    dbglog( "[master] Shutdown received while registering a client. Honouring it.\n" );
                    g_KeepServerRunning = 0;
                    newClientMessage->msg.mn_Node.ln_Type = NT_REPLYMSG;
                    ReplyMsg( clientMessage );
                    CloseSocket( clientSocket );
                    return;
                }
                break;  //Got the child's AEM_NewClient / AEM_KillClient
            }
            Delay( 2 );
        }
        if( newClientMessage == NULL )
        {
            dbglog( "[master] Child did not register within timeout. Abandoning connection.\n" );
            CloseSocket( clientSocket );
            return;
        }
        unsigned short clientPort = newClientMessage->port;

        //Did the child request to kill the client?
        if( newClientMessage->messageType == AEM_KillClient )
        {
            dbglog( "[master] Child requested the termination of new client.\n" );
            CloseSocket( clientSocket );
            ReplyMsg( (struct Message*)newClientMessage );
            dbglog( "[master] Reply to child.\n" );
            return;
        }
        dbglog( "[master] Got the listen port for the new client %d.\n", clientPort );

        //Send a reply
        ReplyMsg( (struct Message*)newClientMessage );
        dbglog( "[master] Reply to child.\n" );

        //Inform the client on which port they should now connect
        ProtocolMessageNewClientPort_t reconnectMessage;
        reconnectMessage.header.token = MAGIC_TOKEN;
        reconnectMessage.header.length = sizeof( reconnectMessage );
        reconnectMessage.header.type = PMT_NEW_CLIENT_PORT;
        reconnectMessage.port = clientPort;

        int bytesSent = 0;
        if( ( bytesSent = sendMessage( SocketBase, clientSocket, (ProtocolMessage_t*)&reconnectMessage ) ) != reconnectMessage.header.length )
        {
            dbglog( "[master] Only sent %d bytes of %d.\n", bytesSent, reconnectMessage.header.length );
        }       

        //Generate a new client entry
        ClientThread_t *clientThread = AllocVec( sizeof( *clientThread ), MEMF_CLEAR|MEMF_FAST );
        clientThread->process = clientProcess;
        clientThread->port = clientPort;
        clientThread->messagePort = clientMessage->mn_ReplyPort;

        //Get the peer name
        struct sockaddr_in name;
        socklen_t nameLen = sizeof( name );
        getpeername( clientSocket, (struct sockaddr *)&name, &nameLen );
        memcpy( clientThread->ip, &name.sin_addr.s_addr, 4 );
        dbglog( "[Master] created thread for IP %u.%u.%u.%u:%d\n", 
                                    (UBYTE)clientThread->ip[ 0 ],
                                    (UBYTE)clientThread->ip[ 1 ],
                                    (UBYTE)clientThread->ip[ 2 ],
                                    (UBYTE)clientThread->ip[ 3 ],
                                    clientThread->port );

        addClientThreadToList( clientThread );
        dbglog( "[master] Done handling the new inbound connection.\n" );

        CloseSocket( clientSocket );
    }
}

//Entry point for the spawned process.
static void clientThread()
{
    clientThreadBody();
    Forbid(); g_ActiveThreadCount--; Permit();
}

static void clientThreadBody()
{
    dbglog( "[child] Client thread started.\n" );

    struct Library *SocketBase = NULL;
    struct sockaddr_in addr __attribute__((aligned(4))) ;
    LONG returnCode = 0;
    unsigned short newPort = g_NextClientPort++;

    //Safety check for the port number
    if( g_NextClientPort > 50000 ) g_NextClientPort = 40000;

    //Find the master server port
    dbglog( "[child] Searching for parent process port.\n" );
    Forbid();
    struct MsgPort *masterPort = FindPort( MASTER_MSGPORT_NAME );
    Permit();
    if( masterPort == NULL )
    {
        dbglog( "[child] Couldn't find master port. Aborting child process.\n" );
        return;
    }

    //Create a reply message port
    struct MsgPort *replyPort = CreateMsgPort();
    if( replyPort == (struct MsgPort *)NULL )
    {
        dbglog( "[child] Failed to create a reply message port.\n" );
        return;
    }

    //Ready our new client message to send to the parent process
    struct AEMessage newClientMessage __attribute__((aligned(4)));
    memset( &newClientMessage, 0, sizeof( newClientMessage ) );
    newClientMessage.msg.mn_Node.ln_Type = NT_MESSAGE;
    newClientMessage.msg.mn_Length = sizeof( struct AEMessage );
    newClientMessage.msg.mn_ReplyPort = replyPort;
    newClientMessage.messageType = AEM_KillClient;
    newClientMessage.port = newPort;

    //Open the BSD Socket library
    dbglog( "[child] Opening bsdsocket.library.\n" );

    SocketBase = OpenLibrary("bsdsocket.library", 4 );
    if( !SocketBase )
    {
        dbglog( "[child] Failed to open the bsdsocket.library.\n" );
        PutMsg( masterPort, (struct Message *)&newClientMessage );
        WaitPort( replyPort );
        DeleteMsgPort( replyPort );
        lockClientThreadList();
        removeClientByPort( newPort );
        unlockClientThreadList();
        dbglog( "[child] Exiting.\n" );
        return;
    }

    //Open a new server port for this client
    dbglog( "[child] Opening client socket.\n" );
    SOCKET childServerSocket = socket(AF_INET, SOCK_STREAM, 0);
    if( childServerSocket == SOCKET_ERROR)
    {
        dbglog( "[child] Error opening client socket.\n" );
        PutMsg( masterPort, (struct Message *)&newClientMessage );
        WaitPort( replyPort );
        DeleteMsgPort( replyPort );
        lockClientThreadList();
        removeClientByPort( newPort );
        unlockClientThreadList();
        dbglog( "[child] Exiting.\n" );
        if( SocketBase != NULL ) { CloseLibrary( SocketBase ); SocketBase = NULL; }
        return;
    }

    dbglog( "[child] Setting socket options.\n" );
    int yes = 1;
    returnCode = setsockopt( childServerSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if(returnCode == SOCKET_ERROR)
    {
        dbglog( "[child] Error setting socket options.\n" );
        PutMsg( masterPort, (struct Message *)&newClientMessage );
        WaitPort( replyPort );
        dbglog( "[child] Exiting.\n" );
        DeleteMsgPort( replyPort );
        lockClientThreadList();
        removeClientByPort( newPort );
        unlockClientThreadList();
        CloseSocket( childServerSocket );
        if( SocketBase != NULL ) { CloseLibrary( SocketBase ); SocketBase = NULL; }
        return;
    }

    //Setting the bind port
    dbglog( "[child] Binding to port %d\n", newPort );
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons( newPort );
    returnCode = bind( childServerSocket, (struct sockaddr *)&addr, sizeof(addr));
    if(returnCode == SOCKET_ERROR)
    {
        dbglog( "[child] Unable to bind to port %d.\n", newPort );
        PutMsg( masterPort, (struct Message *)&newClientMessage );
        WaitPort( replyPort );
        dbglog( "[child] Exiting.\n" );
        DeleteMsgPort( replyPort );
        lockClientThreadList();
        removeClientByPort( newPort );
        unlockClientThreadList();
        CloseSocket( childServerSocket );
        if( SocketBase != NULL ) { CloseLibrary( SocketBase ); SocketBase = NULL; }
        return;
    }

    dbglog( "[child] Enable port listening on port %d.\n", newPort );
    returnCode = listen( childServerSocket, 1 );
    if(returnCode == SOCKET_ERROR)
    {
        dbglog( "[child] Unable to bind to port %d.\n", newPort );
        PutMsg( masterPort, (struct Message *)&newClientMessage );
        WaitPort( replyPort );
        dbglog( "[child] Exiting.\n" );
        DeleteMsgPort( replyPort );
        lockClientThreadList();
        removeClientByPort( newPort );
        unlockClientThreadList();
        CloseSocket( childServerSocket );
        if( SocketBase != NULL ) { CloseLibrary( SocketBase ); SocketBase = NULL; }
        return;
    }

    //Send the socket to the parent process
    dbglog( "[child] Informing the parent which port the client should reconnect on.\n" );
    newClientMessage.messageType = AEM_NewClient;
    PutMsg( masterPort, (struct Message *)&newClientMessage );
    WaitPort( replyPort );

    //Get the socket we will be using
    dbglog( "[child] Waiting for confirmation from parent.\n" );
    struct SocketHandleMessage *portMessage = (struct SocketHandleMessage *)WaitPort( replyPort );
    if( portMessage == NULL )
    {
        dbglog( "[child] Got a null back from parent. Do we care though?\n" );
    }

    //Now wait for the client to connect
    dbglog("[child] Awaiting new connection\n" );

    socklen_t addrLen __attribute__((aligned(4))) = sizeof( addr );

    //Make the listen socket non-blocking
    int nbFlag = 1;
    IoctlSocket( childServerSocket, FIONBIO, &nbFlag );

    SOCKET newClientSocket = -1;
    LONG acceptTimeout = 500;   //~10s at Delay(2)
    while( g_KeepServerRunning && acceptTimeout-- > 0 )
    {
        newClientSocket = (SOCKET)accept( childServerSocket, (struct sockaddr *)&addr, &addrLen );
        if( newClientSocket >= 0 ) break;
        Delay( 2 );
    }
    if( newClientSocket < 0 )
    {
        dbglog( "[child] No client reconnect (server shutdown or timeout). Aborting.\n" );
        goto exit_child;
    }

    //Put the accepted socket back into blocking mode
    int blkFlag = 0;
    IoctlSocket( newClientSocket, FIONBIO, &blkFlag );

    dbglog( "[child] Accepting client connection from handle %d.\n", newClientSocket );

    //Get current buffer sizes
    ULONG sendBufferSize = 0;
    ULONG receiveBufferSize = 0;
    socklen_t varLen = sizeof( sendBufferSize );

    getsockopt( newClientSocket, SOL_SOCKET, SO_RCVBUF, &receiveBufferSize, &varLen );
    getsockopt( newClientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, &varLen );
    dbglog( "[child] Buffer sizes %lu (snd) %lu (rcv)\n", sendBufferSize, receiveBufferSize );

    sendBufferSize = 65536;
    receiveBufferSize = 65536;
    setsockopt( newClientSocket, SOL_SOCKET, SO_RCVBUF, &receiveBufferSize, varLen );
    setsockopt( newClientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, varLen );

    getsockopt( newClientSocket, SOL_SOCKET, SO_RCVBUF, &receiveBufferSize, &varLen );
    getsockopt( newClientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, &varLen );
    dbglog( "[child] Adjusted buffer sizes %lu (snd) %lu (rcv)\n", sendBufferSize, receiveBufferSize );

    //Disable Nagle's algorithm
    static int noDelayYes = 1;
    returnCode = setsockopt( newClientSocket, IPPROTO_TCP, TCP_NODELAY, &noDelayYes, sizeof( noDelayYes ) );
    dbglog( "[child] TCP_NODELAY setsockopt returned %ld\n", returnCode );

    //Enable keep alive
    static int keepAliveYes = 1;
    setsockopt( newClientSocket, SOL_SOCKET, SO_KEEPALIVE, &keepAliveYes, sizeof( keepAliveYes ) );

    //Reserve memory for messages
    ProtocolMessage_t *message __attribute__((aligned(4))) = AllocVec( MAX_MESSAGE_LENGTH, MEMF_FAST|MEMF_CLEAR );

    //Create a filesend context
    FileSendContext_t *fileSendContext = allocateFileSendContext();

    //Current directory lock for this shell session (0 = use task default)
    BPTR cdLock = 0;
    (void)cdLock;

    //Start reading all inbound messages
    int bytesRead = 0;
    volatile char keepThisConnectionRunning = 1;
    LONG bytesAvailable = 0;
    LONG inactivityCount = 300;

    while( keepThisConnectionRunning && g_KeepServerRunning )
    {
        IoctlSocket( newClientSocket, FIONREAD, &bytesAvailable );
        if( bytesAvailable == 0 )
        {
            struct Message *newMessage = GetMsg( replyPort );
            if( newMessage != NULL )
            {
                struct AEMessage *aeMsg = (struct AEMessage *)newMessage;
                if( aeMsg->messageType == AEM_KillClient )
                {
                    dbglog( "[child] Received kill client.\n" );

                    ProtocolMessageDisconnect_t disconnectMessage;
                    disconnectMessage.header.length = sizeof( disconnectMessage );
                    disconnectMessage.header.type = PMT_CLOSING;
                    disconnectMessage.header.token = MAGIC_TOKEN;
                    snprintf( disconnectMessage.message, sizeof( disconnectMessage.message ), "Shutting down server. Sorry, but you are out." );

                    sendMessage( SocketBase, newClientSocket, (ProtocolMessage_t*)&disconnectMessage );
                    dbglog( "[child] Sent disconnect message. Delaying to allow delivery.\n" );

                    dbglog( "[child] Acknowledging the master's request.\n" );
                    ReplyMsg( newMessage );

                    keepThisConnectionRunning = 0;
                    dbglog( "[child] Starting the shutdown.\n" );
                    goto exit_child_clean;
                }
            }

            if( inactivityCount-- == 0 )
            {
                dbglog( "[child] Pinging client due to inactivity.\n" );
                static ProtocolMessage_t ping = { MAGIC_TOKEN, PMT_PING, sizeof( ProtocolMessage_t ) };
                int ret = sendMessage( SocketBase, newClientSocket, &ping );
                if( ret == SOCKET_ERROR )
                {
                    keepThisConnectionRunning = 0;
                    dbglog( "[child] Closing connection due to timeout.\n" );
                }
                else
                {
                    inactivityCount = 300;
                }
            }

            Delay( 5 );
            continue;
        }

        inactivityCount = 300;
        bytesRead = getMessage( SocketBase, newClientSocket, message, MAX_MESSAGE_LENGTH );

        if( bytesRead < 0 )
        {
            ProtocolMessageDisconnect_t disconnectMessage;
            disconnectMessage.header.length = sizeof( disconnectMessage );
            disconnectMessage.header.type = PMT_CLOSING;
            disconnectMessage.header.token = MAGIC_TOKEN;

            switch( bytesRead )
            {
                case MAGIC_TOKEN_MISSING:
                    dbglog( "[child] Magic Token missing in message. Terminating Connection.\n" );
                    snprintf( disconnectMessage.message, sizeof( disconnectMessage.message ), "Magic token in packet was wrong or missing. Disconnecting." );
                    sendMessage( SocketBase, newClientSocket, (ProtocolMessage_t*)&disconnectMessage );
                    keepThisConnectionRunning = 0;
                    continue;
                case INVALID_MESSAGE_TYPE:
                    dbglog( "[child] Received message of an invalid type. Terminating Connection.\n" );
                    snprintf( disconnectMessage.message, sizeof( disconnectMessage.message ), "Server received an invalid message type. Disconnecting." );
                    sendMessage( SocketBase, newClientSocket, (ProtocolMessage_t*)&disconnectMessage );
                    keepThisConnectionRunning = 0;
                    continue;
                case INVALID_MESSAGE_SIZE:
                    dbglog( "[child] Received a message of an invalid size. Terminating Connection.\n" );
                    snprintf( disconnectMessage.message, sizeof( disconnectMessage.message ), "Message size was wrong. Disconnecting." );
                    sendMessage( SocketBase, newClientSocket, (ProtocolMessage_t*)&disconnectMessage );
                    keepThisConnectionRunning = 0;
                    continue;
                case SOCKET_ERROR:
                    dbglog( "[child] Socket error on client connection. Shutting down.\n" );
                    keepThisConnectionRunning = 0;
                    continue;
            }
        }

        if( bytesRead < sizeof( ProtocolMessage_t ) )
        {
            Delay( 5 );
            continue;
        }

        switch( message->type )
        {
            case PMT_SHUTDOWN_SERVER:
                dbglog( "[child] Shutting down the server.\n" );
                g_KeepServerRunning = 0;
                break;

            case PMT_GET_VERSION:
                dbglog( "[child] Server Version Requested.\n" );
                ProtocolMessage_Version_t versionMessage __attribute__((aligned(4))) =
                {
                    .header.token = MAGIC_TOKEN,
                    .header.length = sizeof( ProtocolMessage_Version_t ),
                    .header.type = PMT_VERSION,
                    .major = VERSION_MAJOR,
                    .minor = VERSION_MINOR,
                    .rev = VERSION_REVISION,
                    .releaseType = RELEASE_TYPE
                };
                dbglog( "[child] Sending Version back\n" );
                sendMessage( SocketBase, newClientSocket, (ProtocolMessage_t*)&versionMessage );
                dbglog( "[child] Version sent.\n" );
                break;

            case PMT_GET_DIR_LIST:
            {
                ProtocolMessageGetDirectoryList_t *getDir = (ProtocolMessageGetDirectoryList_t*)message;
                char dirPath[ MAX_FILEPATH_LENGTH ] __attribute__((aligned(4)));
                memset( dirPath, 0, sizeof( dirPath ) );
                memcpy( dirPath, getDir->path, getDir->length );

                if( strlen( dirPath ) == 0 || dirPath[ MAX_FILEPATH_LENGTH - 1 ] != 0 )
                {
                    dbglog( "[child] Ignoring invalid path in GET_DIR_LIST request.\n" );
                    break;
                }

                dbglog( "[child] Directory list for '%s' (%d bytes long) Requested.\n", getDir->path, getDir->length );

                ProtocolMessageDirectoryList_t *list = getDirectoryList( dirPath );
                if( list == NULL )
                {
                    dbglog( "[child] Unable to get directory listing '%s'.\n", getDir->path );
                    char *errorMessage = "That path doesn't exist.";
                    unsigned int messageLength = sizeof( ProtocolMessage_Failed_t ) + strlen( errorMessage );
                    ProtocolMessage_Failed_t *failedMessage = AllocVec( messageLength, MEMF_CLEAR|MEMF_FAST );
                    failedMessage->header.length = messageLength;
                    failedMessage->header.token = MAGIC_TOKEN;
                    failedMessage->header.type = PMT_FAILED;
                    CopyMem( errorMessage, failedMessage->message, strlen( errorMessage ) );
                    sendMessage( SocketBase, newClientSocket, (ProtocolMessage_t*)failedMessage );
                    FreeVec( failedMessage );
                    break;
                }
                sendMessage( SocketBase, newClientSocket, (ProtocolMessage_t*)list );
                break;
            }

            case PMT_GET_FILE:
            {
                ProtocolMessage_FilePull_t *getFileMsg = ( ProtocolMessage_FilePull_t* )message;
                char filePath[ MAX_FILEPATH_LENGTH ] __attribute__((aligned(4)));
                memset( filePath, 0, sizeof( filePath ) );
                strncpy( filePath, getFileMsg->filePath, sizeof( filePath ) - 1 );

                if( strlen( filePath ) == 0 )
                {
                    dbglog( "[child] Ignoring invalid file path in GET_FILE request.\n" );
                    break;
                }

                dbglog( "[child] Get file called for file '%s'\n", filePath );

                ProtocolMessage_Ack_t *acknowledgeMessage = requestFileSend( filePath, fileSendContext );
                if( acknowledgeMessage != NULL )
                {
                    sendMessage( SocketBase, newClientSocket, (ProtocolMessage_t*)acknowledgeMessage );
                    FreeVec( acknowledgeMessage );
                }
                break;
            }

            default:
                dbglog( "[child] Unhandled message type: %d\n", message->type );
                break;
        }
    }

exit_child_clean:
    if( fileSendContext != NULL ) freeFileSendContext( fileSendContext );
    if( message != NULL ) FreeVec( message );

exit_child:
    if( newClientSocket >= 0 ) CloseSocket( newClientSocket );
    if( childServerSocket >= 0 ) CloseSocket( childServerSocket );
    if( replyPort != NULL ) DeleteMsgPort( replyPort );
    if( SocketBase != NULL ) CloseLibrary( SocketBase );

    lockClientThreadList();
    removeClientByPort( newPort );
    unlockClientThreadList();

    dbglog( "[child] Exiting thread body cleanly.\n" );
}