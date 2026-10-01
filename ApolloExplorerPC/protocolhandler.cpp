#include "protocolhandler.h"
#include "messagepool.h"
#include <QtEndian>
#include <QEventLoop>
#include <QDebug>

#define DEBUG 1
#include "AEUtils.h"

ProtocolHandler::ProtocolHandler(QObject *parent) :
    QObject(parent),
    m_AEConnection( this ),
    m_ServerAddress( ),
    m_ServerPort( 30202 ),
    m_ConnectionPhase( CP_DISCONNECTED )
{
    //setup signal slots
    connect( &m_AEConnection, &AEConnection::connectedToHostSignal, this, &ProtocolHandler::onConnectedSlot );
    connect( &m_AEConnection, &AEConnection::disconnectedFromHostSignal, this, &ProtocolHandler::onDisconnectedSlot );
    connect( &m_AEConnection, &AEConnection::newMessageReceived, this, &ProtocolHandler::onMessageReceivedSlot );
    connect( this, &ProtocolHandler::connectToHostSignal, &m_AEConnection, &AEConnection::onConnectToHostRequestedSlot );
    connect( this, &ProtocolHandler::disconnectFromHostSignal, &m_AEConnection, &AEConnection::onDisconnectFromhostRequestedSlot );

    //Through put signals
    connect( &m_AEConnection, &AEConnection::outgoingByteCountSignal, this, &ProtocolHandler::outgoingByteCountSignal );
    connect( &m_AEConnection, &AEConnection::incomingByteCountSignal, this, &ProtocolHandler::incomingByteCountSignal );

    //Raw Socket
    connect( this, &ProtocolHandler::rawOutgoingBytesSignal, &m_AEConnection, &AEConnection::onRawOutgoingBytesSlot );
    connect( &m_AEConnection, &AEConnection::rawIncomingBytesSignal, this, &ProtocolHandler::onRawIncomingBytesSlot );
}

void ProtocolHandler::onConnectToHostRequestedSlot( QHostAddress serverAddress, quint16 port )
{
    DBGLOG << "Connecting to host " << serverAddress << " on port " << port;
    //Save this for the reconnect later.
    m_ServerAddress = serverAddress;
    m_ServerPort = port;

    //We will now enter the initial connection phase
    m_ConnectionPhase = CP_CONNECTING;

    //Send this on to the connection
    emit connectToHostSignal( serverAddress, port );
}

void ProtocolHandler::onDisconnectFromHostRequestedSlot()
{
    emit disconnectFromHostSignal();
}

void ProtocolHandler::onSendMessageSlot( ProtocolMessage_t *message )
{
    m_AEConnection.onSendMessage( message );
}

void ProtocolHandler::onSendAndReleaseMessageSlot( ProtocolMessage_t *message )
{
    m_AEConnection.onSendMessage( message );
    ReleaseMessage( message );
}

void ProtocolHandler::onDeleteFileSlot(QString remotePath)
{
    //Form the file deletion message
    ProtocolMessage_DeletePath_t *deleteMessage = AllocMessage<ProtocolMessage_DeletePath_t>();
    deleteMessage->header.type = PMT_DELETE_PATH;
    deleteMessage->header.token = MAGIC_TOKEN;
    deleteMessage->header.length = sizeof( *deleteMessage ) + remotePath.size();
    deleteMessage->entryType = qToBigEndian( (unsigned int)DET_FILE );
    memset( deleteMessage->filePath, 0, MAX_FILEPATH_LENGTH );
    convertFromUTF8ToAmigaTextEncoding( remotePath, deleteMessage->filePath, remotePath.length() );

    //Send the message
    m_AEConnection.sendMessage( deleteMessage );
    ReleaseMessage( deleteMessage );
}

void ProtocolHandler::onDeleteRecursiveSlot(QString remotePath)
{
    //Form the file deletion message
    ProtocolMessage_DeletePath_t *deleteMessage = AllocMessage<ProtocolMessage_DeletePath_t>();
    deleteMessage->header.type = PMT_DELETE_PATH;
    deleteMessage->header.token = MAGIC_TOKEN;
    deleteMessage->header.length = sizeof( *deleteMessage ) + remotePath.size();
    deleteMessage->entryType = qToBigEndian( (unsigned int)DET_USERDIR );
    memset( deleteMessage->filePath, 0, MAX_FILEPATH_LENGTH );
    convertFromUTF8ToAmigaTextEncoding( remotePath, deleteMessage->filePath, remotePath.length() );

    //Send the message
    m_AEConnection.sendMessage( deleteMessage );
    ReleaseMessage( deleteMessage );
}

void ProtocolHandler::onCancelDeleteDirectorySlot()
{
    //TODO:
}

void ProtocolHandler::onGetVolumeListSlot()
{
    ProtocolMessage_t *getVolumeList = NewMessage();
    if( getVolumeList )
    {
            //Send the query
            getVolumeList->token = MAGIC_TOKEN;
            getVolumeList->type = PMT_GET_VOLUMES;
            getVolumeList->length = sizeof( ProtocolMessage_t );
            this->onSendMessageSlot( getVolumeList );
            FreeMessage( getVolumeList );
    }
}

void ProtocolHandler::onRunCMDSlot(QString command, QString workingDirectroy)
{
    Q_UNUSED( workingDirectroy )
    //Create the command message
    ProtocolMessage_Run_t *runMessage = AllocMessage<ProtocolMessage_Run_t>();
    runMessage->header.token = MAGIC_TOKEN;
    runMessage->header.type = PMT_RUN;
    runMessage->header.length = sizeof( ProtocolMessage_Run_t ) + command.length() + 1;
    strncpy( runMessage->command, command.toStdString().c_str(), command.length() + 1 );

    //Send the message
    m_AEConnection.sendMessage( runMessage );

    //Free Message
    ReleaseMessage( runMessage );

    //Enable raw mode
    //m_VNetConnection.onSetRawSocketMode();
}

void ProtocolHandler::onGetDirectorySlot( QString remoteDirectory )
{
    //Convert the text encoding
    char encodedPath[ MAX_FILEPATH_LENGTH ];
    convertFromUTF8ToAmigaTextEncoding( remoteDirectory, encodedPath, sizeof( encodedPath ) );

    ProtocolMessageGetDirectoryList_t *getDirMsg = AllocMessage<ProtocolMessageGetDirectoryList_t>();
    if( getDirMsg )
    {
        DBGLOG << "Getting dir path " << remoteDirectory;

        //Setup the message
        getDirMsg->length = strlen( encodedPath );
        memcpy( getDirMsg->path, encodedPath, getDirMsg->length );
        getDirMsg->path[ getDirMsg->length ] = 0;
        getDirMsg->header.token = MAGIC_TOKEN;
        getDirMsg->header.type = PMT_GET_DIR_LIST;
        getDirMsg->header.length = sizeof( ProtocolMessageGetDirectoryList_t ) + getDirMsg->length + 1;

        //Lastly, some endian conversion
        getDirMsg->length = qToBigEndian<unsigned int>( getDirMsg->length );

        //Send the message
        m_AEConnection.sendMessage( getDirMsg );

        //Free the message
        ReleaseMessage( getDirMsg );
    }
}

void ProtocolHandler::onMKDirSlot( QString remoteDirectory )
{
    //Convert the text encoding
    char encodedPath[ MAX_FILEPATH_LENGTH ];
    convertFromUTF8ToAmigaTextEncoding( remoteDirectory, encodedPath, sizeof( encodedPath ) );

    //Form the message
    ProtocolMessage_MakeDir_t *mkdirMessage = AllocMessage<ProtocolMessage_MakeDir_t>();
    mkdirMessage->header.token = MAGIC_TOKEN;
    mkdirMessage->header.type = PMT_MKDIR;
    mkdirMessage->header.length = sizeof( ProtocolMessage_MakeDir_t ) + remoteDirectory.length();
    strncpy( mkdirMessage->filePath, encodedPath, strlen( encodedPath ) + 1 );

    //Send the message
    m_AEConnection.sendMessage( mkdirMessage );
    ReleaseMessage( mkdirMessage );
}

void ProtocolHandler::onRenameFileSlot(QString oldPathName, QString newPathName)
{
    //Convert the text encoding
    char encodedOldName[ MAX_FILEPATH_LENGTH ];
    char encodedNewName[ MAX_FILEPATH_LENGTH ];
    convertFromUTF8ToAmigaTextEncoding( oldPathName, encodedOldName, sizeof( encodedOldName ) );
    convertFromUTF8ToAmigaTextEncoding( newPathName, encodedNewName, sizeof( encodedNewName ) );

    //Form the message
    ProtocolMessage_RenamePath_t *renameMessage = AllocMessage<ProtocolMessage_RenamePath_t>();
    renameMessage->header.token = MAGIC_TOKEN;
    renameMessage->header.type = PMT_RENAME_FILE;
    renameMessage->oldNameSize = oldPathName.length();
    renameMessage->newNameSize = newPathName.length();
    renameMessage->header.length = sizeof( ProtocolMessage_MakeDir_t ) + oldPathName.length() + newPathName.length() + 2;
    strncpy( renameMessage->filePaths, encodedOldName, strlen( encodedOldName ) + 1 );
    strncpy( renameMessage->filePaths + renameMessage->oldNameSize + 1, encodedNewName, strlen( encodedNewName ) + 1 );

    //NOw we should endian convert the sizes
    renameMessage->oldNameSize = qToBigEndian( renameMessage->oldNameSize );
    renameMessage->newNameSize = qToBigEndian( renameMessage->newNameSize );

    //Send the message
    m_AEConnection.sendMessage( renameMessage );
    ReleaseMessage( renameMessage );
}

bool ProtocolHandler::deleteFile( QString remoteFilePath, QString &error )
{
    //Convert the text encoding
    char encodedPath[ MAX_FILEPATH_LENGTH ];
    convertFromUTF8ToAmigaTextEncoding( remoteFilePath, encodedPath, sizeof( encodedPath ) );

    //Form the message
    ProtocolMessage_DeletePath_t *deletePathMessage = AllocMessage<ProtocolMessage_DeletePath_t>();
    deletePathMessage->header.token = MAGIC_TOKEN;
    deletePathMessage->header.type = PMT_DELETE_PATH;
    deletePathMessage->header.length = sizeof( ProtocolMessage_DeletePath_t ) + remoteFilePath.length();
    strncpy( deletePathMessage->filePath, encodedPath, strlen( encodedPath ) + 1 );

    //Send the message
    m_AEConnection.sendMessage( deletePathMessage );
    ReleaseMessage( deletePathMessage );

    //We want to wait for the OK or error message
    quint32 waitTime = 5000;
    QTimer timer;
    timer.setSingleShot(true);
    QEventLoop loop;
    static bool succeeded = false;
    static bool deleteCompleted = false;
    connect( this, &ProtocolHandler::acknowledgeSignal, &loop, &QEventLoop::quit );
    connect( this, &ProtocolHandler::failedSignal, &loop, &QEventLoop::quit );
    connect( this, &ProtocolHandler::acknowledgeSignal, [&]()
            {
                succeeded = true;
                deleteCompleted = true;
            }
            );
    connect( this, &ProtocolHandler::failedSignal, [&]()
            {
                succeeded = false;
                deleteCompleted = true;
            }
            );
    connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
    timer.start( waitTime );

    //In case multiple parallel getDir() operations are going on, we should wait for the right one
    while( deleteCompleted == false )
    {
        loop.exec();
        if( !timer.isActive() )
            break;
    }

    //Did we timeout?
    if( !timer.isActive() )
    {
        error = "Operation Timed out";
        return false;
    }
    timer.stop();

    //Return our success or not
    return succeeded;
}

void ProtocolHandler::onConnectedSlot()
{
    if( m_ConnectionPhase == CP_RECONNECTING )
    {
        //Now we are connected
        m_ConnectionPhase = CP_CONNECTED;

        //We should ask for the version
        ProtocolMessage_t *versionQuery = AllocMessage<ProtocolMessage_t>();
        if( versionQuery )
        {
                //Send the query
                versionQuery->token = MAGIC_TOKEN;
                versionQuery->type = PMT_GET_VERSION;
                versionQuery->length = sizeof( ProtocolMessage_t );
                m_AEConnection.sendMessage( versionQuery );
                ReleaseMessage( versionQuery );
        }
    }

    //emit connectedToHostSignal();
}

void ProtocolHandler::onDisconnectedSlot()
{
    if( m_ConnectionPhase == CP_CONNECTED )
    {
        m_ConnectionPhase = CP_DISCONNECTED;
        emit disconnectedFromHostSignal();
    }
}

void ProtocolHandler::onMessageReceivedSlot( quint32 messageType, char *newMessage )
{
    if( !newMessage )
        return;

    switch( messageType )
    {
        case PMT_FILE_CHUNK:
        {
            ProtocolMessage_FileChunk_t *fileChunkMsg = reinterpret_cast<ProtocolMessage_FileChunk_t*>( newMessage );

            // Extraer metadatos del paquete en formato de red a host
            quint32 chunkNumber = qFromBigEndian<quint32>( fileChunkMsg->chunkNumber );
            quint64 bytes = qFromBigEndian<quint32>( fileChunkMsg->bytesContained );
            
            // Extracción del checksum FNV-1a enviado por el servidor Amiga
            // (Asegúrate de que tu estructura C incluya este campo al final)
            quint32 serverChecksum = qFromBigEndian<quint32>( fileChunkMsg->checksum ); 
            
            QByteArray chunk( fileChunkMsg->chunk, bytes );

            // Emitir la señal hacia el hilo de descarga con el checksum recibido
            emit fileChunkSignal( chunkNumber, bytes, chunk, serverChecksum ); 
            break;
        }
        
        // ... (otros tipos de mensajes del protocolo)
        
        default:
            #if DEBUG
            DBGLOG << "Unknown message type received: " << messageType;
            #endif
            break;
    }
}

void ProtocolHandler::onRawOutgoingBytesSlot(QByteArray bytes)
{
    emit rawOutgoingBytesSignal( bytes );
}

void ProtocolHandler::onRawIncomingBytesSlot(QByteArray bytes)
{
    emit rawIncomingBytesSignal( bytes );
}
