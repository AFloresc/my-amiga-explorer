/*
 * FileSend.c
 *
 *  Created on: May 23, 2021
 *      Author: rony
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocolTypes.h"
#include "protocol.h"
#include "SendFile.h"

#include <sys/socket.h>
#include <sys/select.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#define DBGOUT 0


#ifdef __GNUC__
#if DBGOUT
#include <stdio.h>
#endif
#include <string.h>
#include <proto/dos.h>
#include <proto/exec.h>
#endif

#ifdef __VBCC__

#endif

#include "AEUtil.h"
#include "protocol.h"
#include "protocolTypes.h"

#if 0
static BPTR g_FileLock = (BPTR)NULL;
static BPTR g_FileHandle = (BPTR)NULL;
static struct FileInfoBlock g_FileInfoBlock;
static ULONG g_CurrentChunk = 0;
static ULONG g_TotalChunks = 0;

//file reading book keeping
static LONG g_TotalBytesLeftToRead = 0;
static LONG g_TotalBytesRead = 0;

//So we don't waste to much time (re)allocating for messages we will (re)use often, we create them once
static ProtocolMessage_FileChunk_t *g_FileChunkMessage = NULL;
static ProtocolMessage_Ack_t *g_AcknowledgeMessage = NULL;
static ProtocolMessage_StartOfFileSend_t *g_StartOfFilesendMessage = NULL;
#endif

#define FNV_PRIME_32        16777619UL
#define FNV_OFFSET_BASIS_32 2166136261UL

ULONG CalculateFNV1a32(const UBYTE *data, ULONG length)
{
    ULONG hash = FNV_OFFSET_BASIS_32;
    ULONG i;

    for (i = 0; i < length; i++)
    {
        hash ^= (ULONG)data[i];
        hash *= FNV_PRIME_32;
    }

    return hash;
}

ProtocolMessage_Ack_t *requestFileSend( char *path, FileSendContext_t *context )
{
    //Valid context?
    if( context == NULL )
    {
        //We are currently doing a file send
        dbglog( "Invalid context for file send!\n" );
        return NULL;
    }

    //Set up the message
    context->acknowledgeMessage->header.token = MAGIC_TOKEN;
    context->acknowledgeMessage->header.length = sizeof( ProtocolMessage_Ack_t );
    context->acknowledgeMessage->header.type = PMT_ACK;

    //First check if this file exists
    dbglog( "[requestFileSend] Checking for the existance of '%s'.\n", path );
    dbglog( "[requestFileSend] Current g_AcknowledgeMessage address 0x%08x.\n", (int)context->acknowledgeMessage );
    context->fileLock = Lock( path, ACCESS_READ );
    if( context->fileLock == (BPTR)NULL )
    {
        //We couldn't get a file lock
        context->acknowledgeMessage->response = 0;
        dbglog( "[requestFileSend] File '%s' is unreadable or doesn't exist.\n", path );
    }else
    {
        context->acknowledgeMessage->response = 1;
        dbglog( "[requestFileSend] File '%s' is readable.\n", path );
        UnLock( context->fileLock );
        context->fileLock = (BPTR)NULL;
    }

    dbglog( "[requestFileSend] Acknowledge response set to %d.\n", context->acknowledgeMessage->response );

    return context->acknowledgeMessage;
}

ProtocolMessage_StartOfFileSend_t *getStartOfFileSend( char *path, FileSendContext_t *context )
{
    //Do we have a valid context?
    if( context == NULL )
    {
        dbglog( "Invalid context.  Shame the user will never know.\n" );
        return NULL;
    }

    //Reset the variable parts of the message
    strncpy( context->startOfFilesendMessage->filePath, path, MAX_FILEPATH_LENGTH );
    context->startOfFilesendMessage->fileSize = 0;
    context->startOfFilesendMessage->numberOfFileChunks = 0;
    context->startOfFilesendMessage->byteOffset = 0;

    //Let's find out how big the file is first
    context->fileHandle = Open( path, MODE_OLDFILE );
    if( context->fileHandle == (BPTR)NULL )
    {
        dbglog( "[getStartOfFile] Failed to open file '%s' for reading.\n", path );
        return context->startOfFilesendMessage;
    }

    //Examine the file in question
    if( !ExamineFH( context->fileHandle, &context->fileInfoBlock ) )
    {
        dbglog( "[getStartOfFile] Failed to examine file '%s' for reading.\n", path );
        Close( context->fileHandle );
        context->fileHandle = (BPTR)NULL;
        return context->startOfFilesendMessage;
    }

    //So how many blocks do we send?
    context->totalChunks = context->fileInfoBlock.fib_Size / FILE_CHUNK_SIZE + ( context->fileInfoBlock.fib_Size%FILE_CHUNK_SIZE > 0 ? 1 : 0);
    context->currentChunk = 0;

    context->startOfFilesendMessage->fileSize = context->fileInfoBlock.fib_Size;
    context->startOfFilesendMessage->numberOfFileChunks = context->totalChunks;
    context->startOfFilesendMessage->byteOffset = 0;

    dbglog( "[getStartOfFile] Filesize: %d.\n", context->startOfFilesendMessage->fileSize );
    dbglog( "[getStartOfFile] Chunks: %d.\n", context->startOfFilesendMessage->numberOfFileChunks );

    //We are done here
    return context->startOfFilesendMessage;
}

ProtocolMessage_FileChunk_t *getNextFileSendChunk( char *path, FileSendContext_t *context )
{
    int bytesRead = 0;

    //Valid context?
    if( context == NULL )
    {
        dbglog( "Invalid context.  Shame the user will never know.\n" );
        return NULL;
    }

    //Clear out the current message
    memset( context->fileChunkMessage->chunk, 0, FILE_CHUNK_SIZE );

    //Read the next file data
    bytesRead = Read( context->fileHandle, context->fileChunkMessage->chunk, FILE_CHUNK_SIZE );
    if( bytesRead < 0 )
    {
        dbglog( "[getNextFileSendChunk] Reading of file '%s' failed.\n", path );
        Close( context->fileHandle );
        context->fileHandle = (BPTR)NULL;
        return NULL;
    }
    if( bytesRead == 0 )
    {
        //End-of-file
        dbglog( "[getNextFileSendChunk] Reached the end of file '%s'.\n", path );
        Close( context->fileHandle );
        context->fileHandle = (BPTR)NULL;
        return NULL;
    }
    dbglog( "[getNextFileSendChunk] Read %d bytes from file '%s'.\n", bytesRead, path );

    //Update book keeping
    context->totalBytesLeftToRead -= bytesRead;
    context->totalBytesRead += bytesRead;

    //Update the chunk message
    context->fileChunkMessage->bytesContained = bytesRead;
    context->fileChunkMessage->chunkNumber = context->currentChunk++;

    //send
    return context->fileChunkMessage;
}

void cleanupFileSend( FileSendContext_t *context )
{
    //Valid context?
    if( context == NULL ) return;

    //Close any file we have open
    if( context->fileHandle != (BPTR)NULL )
    {
        Close( context->fileHandle );
        context->fileHandle = (BPTR)NULL;
    }

    //clean up book keeping
    context->totalBytesLeftToRead = 0;
    context->totalBytesRead = 0;
    context->currentChunk = 0;
    context->totalChunks = 0;
}

FileSendContext_t *allocateFileSendContext()
{
    FileSendContext_t *context = (FileSendContext_t*)AllocVec( sizeof( FileSendContext_t ), MEMF_CLEAR|MEMF_FAST );
    if( context == NULL ) return NULL;

    //Allocate the acknowledge message
    context->acknowledgeMessage = AllocVec( sizeof( ProtocolMessage_Ack_t ), MEMF_FAST|MEMF_CLEAR );
    context->acknowledgeMessage->header.token = MAGIC_TOKEN;
    context->acknowledgeMessage->header.length = sizeof( ProtocolMessage_Ack_t );
    context->acknowledgeMessage->header.type = PMT_ACK;

    //Allocate the start of send message
    context->startOfFilesendMessage = ( ProtocolMessage_StartOfFileSend_t* )AllocVec( MAX_MESSAGE_LENGTH, MEMF_FAST|MEMF_CLEAR );
    context->startOfFilesendMessage->header.token = MAGIC_TOKEN;
    context->startOfFilesendMessage->header.length = sizeof( ProtocolMessage_StartOfFileSend_t );
    context->startOfFilesendMessage->header.type = PMT_START_OF_SEND_FILE;

    //Allocate the file chunk message
    context->fileChunkMessage = AllocVec( sizeof( ProtocolMessage_FileChunk_t ), MEMF_FAST|MEMF_CLEAR );
    context->fileChunkMessage->header.token = MAGIC_TOKEN;
    context->fileChunkMessage->header.length = sizeof( ProtocolMessage_FileChunk_t );
    context->fileChunkMessage->header.type = PMT_FILE_CHUNK;

    return context;
}

void freeFileSendContext( FileSendContext_t *context )
{
    //Is it a valid pointer?
    if( context == NULL )  return;

    //Make sure we have cleaned up after ourselves
    cleanupFileSend( context );

    //Free everything
    if( context->acknowledgeMessage ) FreeVec( context->acknowledgeMessage );
    if( context->startOfFilesendMessage ) FreeVec( context->startOfFilesendMessage );
    if( context->fileChunkMessage ) FreeVec( context->fileChunkMessage );
    FreeVec( context );
}

BOOL SendFile(LONG socketFd, STRPTR localFilePath, unsigned int byteOffset)
{
    BPTR fileHandle = Open(localFilePath, MODE_OLDFILE);
    if (!fileHandle)
    {
        return FALSE;
    }

    // Obtener el tamaño total del archivo usando Seek al final
    LONG fileSize = Seek(fileHandle, 0, OFFSET_END);
    if (fileSize == -1)
    {
        dbglog("[SendFile] Error al obtener el tamaño del archivo '%s'\n", localFilePath);
        Close(fileHandle);
        return FALSE;
    }

    // Gestión del offset para reanudación
    if (byteOffset > 0)
    {
        if ((unsigned int)fileSize <= byteOffset)
        {
            Close(fileHandle);
            return TRUE; 
        }
        Seek(fileHandle, (LONG)byteOffset, OFFSET_BEGINNING);
    }
    else
    {
        Seek(fileHandle, 0, OFFSET_BEGINNING);
    }

    // Reservar memoria para la estructura completa del mensaje
    ProtocolMessage_FileChunk_t *chunkMsg = AllocVec(sizeof(ProtocolMessage_FileChunk_t), MEMF_FAST | MEMF_CLEAR);
    if (!chunkMsg)
    {
        Close(fileHandle);
        return FALSE;
    }

    ULONG currentChunkIndex = 0;
    LONG bytesRead = 0;
    BOOL aborted = FALSE;

    while ((bytesRead = Read(fileHandle, chunkMsg->chunk, FILE_CHUNK_SIZE)) > 0)
    {
        /* Comprobación de cancelación por parte del cliente */
        struct timeval tv = { 0, 0 };
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socketFd, &readfds);

        if (WaitSelect(socketFd + 1, &readfds, NULL, NULL, &tv, NULL) > 0)
        {
            ProtocolMessage_t header;
            if (recv(socketFd, (char *)&header, sizeof(ProtocolMessage_t), MSG_PEEK) >= (LONG)sizeof(ProtocolMessage_t))
            {
                ULONG msgType = header.type;
                if (msgType == PMT_CANCEL_OPERATION || ntohl(msgType) == PMT_CANCEL_OPERATION)
                {
                    recv(socketFd, (char *)&header, sizeof(ProtocolMessage_t), 0);
                    aborted = TRUE;
                    dbglog("[SendFile] Transferencia cancelada por el cliente.\n");
                    break;
                }
            }
        }

        /* Rellenar cabecera y metadatos del chunk */
        chunkMsg->header.token = MAGIC_TOKEN;
        chunkMsg->header.type = PMT_FILE_CHUNK;
        chunkMsg->header.length = sizeof(ProtocolMessage_FileChunk_t);
        chunkMsg->chunkNumber = currentChunkIndex++;
        chunkMsg->bytesContained = bytesRead;

        /* Calcular el Checksum FNV-1a */
        chunkMsg->checksum = CalculateFNV1a32((UBYTE *)chunkMsg->chunk, bytesRead);

        #if DBGOUT
        dbglog("[SendFile] Chunk %lu - Bytes: %ld, Checksum FNV-1a: 0x%08lx\n", chunkMsg->chunkNumber, bytesRead, chunkMsg->checksum);
        #endif

        /* Enviar la estructura entera por el socket */
        if (send(socketFd, (char *)chunkMsg, sizeof(ProtocolMessage_FileChunk_t), 0) != sizeof(ProtocolMessage_FileChunk_t))
        {
            aborted = TRUE;
            break;
        }
    }

    Close(fileHandle);
    FreeVec(chunkMsg);
    

    if (aborted)
    {
        ProtocolMessage_Ack_t ackMsg;
        memset(&ackMsg, 0, sizeof(ackMsg));
        ackMsg.header.token = MAGIC_TOKEN;
        ackMsg.header.type = PMT_ACK;
        ackMsg.header.length = sizeof(ProtocolMessage_Ack_t);
        ackMsg.response = AT_NOK;

        send(socketFd, (char *)&ackMsg, sizeof(ProtocolMessage_Ack_t), 0);
        return FALSE;
    }

    return TRUE;
}