/*
 * peekpoke.c -- the meat of our "simple" peek and poke web server
 */

/* Standard includes. */
#include <stdio.h>
#include <stdlib.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"

/* FreeRTOS Protocol includes. */
#include "FreeRTOS_HTTP_commands.h"
#include "FreeRTOS_TCP_server.h"
#include "FreeRTOS_server_private.h"

/* Sim calls */
#include "sbb_freertos.h"

/* Specifics for the peekpoke server. */
#include "peekpoke.h"

/* First, some helper functions. */

static uint8_t *addressToUInt8Ptr( long long int address )
{
    /* convert first to unsigned 32-bit int */
    unsigned int tmp = address & 0xffffffff;
    return (uint8_t *) tmp; /* evil! */
}

/* and the malware function */

#define NOP portNOP();
#define NOP4 NOP NOP NOP NOP
#define NOP16 NOP4 NOP4 NOP4 NOP4
#define NOP64 NOP16 NOP16 NOP16 NOP16
#define NOP256 NOP64 NOP64 NOP64 NOP64
#define NOP1024 NOP256 NOP256 NOP256 NOP256
#define NOP4096 NOP1024 NOP1024 NOP1024 NOP1024

static size_t malware (uint8_t *ptr, size_t num) {
    (void) ptr;
    (void) num;
    
    NOP256;
    NOP4096;
    NOP256;
    
    return 0;
}

// start and end of the malware function body; note that it includes
// 4096 NOPs plus a return, each of which is 32 bits (4 bytes) long,
// plus buffers on both sides - we're effectively providing a region
// of 4352 NOPs, with a bunch of NOPs and function frame setup on
// either side
static const uint8_t *malware_region_start = ((uint8_t *) &malware) + 128 * 4;
static const uint8_t *malware_region_end = ((uint8_t *) &malware) + (128 + 4096) * 4;


/* Stateful functions to split a slash-separated string of numbers. */

static const char *urlBuf = NULL;
static const char *otherData = NULL;
static int endOfHeadersSeen = 0;

/*
 * Initializes our hacky HTTP request processor with the URL and
 * the "other data" (basically, everything starting with "HTTP/1.1"
 * and heading into the headers and, if present, message body.
 */
static void loadRequestData(const char* urlBuf_, const char* otherData_)
{
    urlBuf = urlBuf_;
    otherData = otherData_;
    endOfHeadersSeen = 0;
}

/*
 * Given a URL containing slash-separated numeric strings, returns
 * the next number and advances further into the URL.
 */
static long long int getNextNumberFromURL(void)
{
    /* 
     * Infinite list of todo items that will never happen here:
     * - Unicode support
     * - Error handling for non-integer inputs
     * - Integers that are too big, so have overflows/wraparounds
     */

    if ( urlBuf == NULL )
    {
        return 0;
    }

    char *endPtr = NULL;
    long long int result = strtoll(urlBuf, &endPtr, 0); // overwrites endPtr

    if ( endPtr == NULL )
    {
        urlBuf = NULL;
    }
    else if ( *endPtr == '/' )
    {
        /*
         * This is the desired state: the first "invalid" character
         * is the slash character, so we'll start again next time one
         * character beyond it. 
         */
        urlBuf = endPtr + 1;
    }
    else if ( endPtr == urlBuf ) {
        /* No valid number found at all! Note absence of error handling. */
        urlBuf = NULL;
        result = 0;
    }
    else
    {
        /* Probably at the 0-terminated end of the string, i.e, *endPtr = 0 */
        urlBuf = endPtr;
    }

    return result;
}

#define BUF_SIZE 1024
static char outputBuf[BUF_SIZE]; /* worries about buffer overflows here? ha! */

/*
 * Returns string for the header line, with the customary termination (\r\n)
 * removed. The same internal buffer is used every time, so make a copy if
 * you need it. Returns NULL when the headers are done.
 */
static char* getNextHttpHeader( void )
{
    if ( endOfHeadersSeen ) return NULL;
    
    char* next = outputBuf;
    for (;;)
    {
        switch ( *otherData )
        {
        case '\0':
            // FreeRTOS_debug_printf(("0-char found, end of input\r\n"));
            endOfHeadersSeen = 1;
            return outputBuf;
        case '\r':
            // FreeRTOS_debug_printf(("carriage-return found\r\n"));
            if ( otherData[1] == '\n' )
            {
                // FreeRTOS_debug_printf(("newline found\r\n"));
                otherData = otherData + 2; // skip over \r\n
                *next = '\0';
                if ( next == outputBuf ) // blank line, end of headers
                {
                    // FreeRTOS_debug_printf(("blank line found, end of input\r\n"));
                    endOfHeadersSeen = 1;
                    return NULL;
                }
                return outputBuf;
            }
            // fallthrough
        default:
            *next = otherData[0];
            next++;
            otherData++;
        }
    }
}

static const char* getHttpBody( void )
{
    char* header;
    
    while ( (header = getNextHttpHeader() ) != NULL )
    {
        FreeRTOS_debug_printf(("Skipping header: %s\r\n", header));
    }
    return otherData;
}

static char heapBuffer[BUF_SIZE] = "plugh";

size_t peekPokeHandler( HTTPClient_t *pxClient, BaseType_t xIndex, const char *pcURLData, char *pcOutputBuffer, size_t uxBufferLength )
{
    char stackBuffer[BUF_SIZE] = "xyzzy";
    
    switch ( xIndex )
    {
    case ECMD_GET:
        // could be "/hello" or "/peek/address/length" or "/run/pointer/integer"
        if ( 0 == strncmp( "/hello", pcURLData, 6 ) )
        {
            strcpy( pxClient->pxParent->pcContentsType, "text/plain" );

            /* useful for a hacker to have a stack addr */
            snprintf( pcOutputBuffer, uxBufferLength,
                      "It's dark here; you may be eaten by a grue.\n\n&stackBuffer = %p\n&heapBuffer = %p\nBUF_SIZE = %d\nuxBufferLength = %d\nstackBuffer = %s\nheapBuffer = %s\nentryAddress = %p\navailableBytes = %td\n",
                      &stackBuffer, &heapBuffer, BUF_SIZE, uxBufferLength,
                      stackBuffer, heapBuffer, malware_region_start,
                      (malware_region_end - malware_region_start) );


            /* all this print logging to help debug the HTTP header processing */

            FreeRTOS_debug_printf(("GET %s\r\n", pcURLData));

            char *tmp;
            while ( (tmp = getNextHttpHeader()) != NULL )
            {
                FreeRTOS_debug_printf(("==> %s\r\n", tmp));
            }

            FreeRTOS_debug_printf(("BODY\r\n%s", getHttpBody()));

            return strlen( pcOutputBuffer );
        }
        else if ( 0 == strncmp( "/peek/", pcURLData, 6 ) )
        {
            strcpy( pxClient->pxParent->pcContentsType, "application/octet-stream" );

            loadRequestData(pcURLData + 6, pxClient->pcRestData);
            long long int memAddress = getNextNumberFromURL();
            size_t readLength = getNextNumberFromURL();
            const uint8_t *mem = addressToUInt8Ptr( memAddress );

            if ( memAddress != 0 && readLength != 0 )
            {
                if ( readLength > uxBufferLength )
                {
                    readLength = uxBufferLength; /* best we can do */
                }

                bzero( pcOutputBuffer, uxBufferLength ); 
                memcpy( pcOutputBuffer, mem, readLength ); /* evil! */

                return readLength;
            }
        }
        else if ( 0 == strncmp( "/run/", pcURLData, 5 ) )
        {
            strcpy( pxClient->pxParent->pcContentsType, "application/octet-stream" );

            loadRequestData(pcURLData + 5, pxClient->pcRestData);
            long long int malware_pointer_address = getNextNumberFromURL();
            size_t malware_int = getNextNumberFromURL();
            uint8_t *malware_pointer = addressToUInt8Ptr( malware_pointer_address );

            // execute the malware function with the specified parameters
            size_t ret = malware(malware_pointer, malware_int);
            snprintf( pcOutputBuffer, uxBufferLength, "return val: %u\n", ret);
            return strlen( pcOutputBuffer );
        }
#ifdef SIMULATION
        // allow triggering of simulation events via HTTP
        else if ( 0 == strncmp( "/sim/", pcURLData, 5 ) )
        {
            static char * unknown_cmd = "unknown_cmd";
            static char * paper_in_pressed = "paper_in_pressed";
            static char * paper_in_released = "paper_in_released";
            static char * scan_barcode_1 = "scan_barcode_1";
            static char * scan_barcode_2 = "scan_barcode_2";
            static char * scan_barcode_3 = "scan_barcode_3";
            static char * press_button_cast = "press_button_cast";
            static char * pres_button_spoil = "pres_button_spoil";
            const char * cmd = pcURLData+5;
            char * reply;

            strcpy( pxClient->pxParent->pcContentsType, "text/plain" );

            if ( 0 == strncmp( paper_in_pressed, cmd, strlen(paper_in_pressed) ) ) 
            {
                reply = paper_in_pressed;
                sim_paper_sensor_in_pressed();
            }
            else if ( 0 == strncmp( paper_in_released, cmd, strlen(paper_in_released) ) ) 
            {
                reply = paper_in_released;
                sim_paper_sensor_in_released();
            }
            else if ( 0 == strncmp( scan_barcode_1, cmd, strlen(scan_barcode_1) ) )
            {
                reply = scan_barcode_1;
                sim_valid_barcode_scanned(1);
            }
            else if ( 0 == strncmp( scan_barcode_2, cmd, strlen(scan_barcode_2) ) )
            {
                reply = scan_barcode_2;
                sim_valid_barcode_scanned(2);
            }
            else if ( 0 == strncmp( scan_barcode_3, cmd, strlen(scan_barcode_3) ) )
            {
                reply = scan_barcode_3;
                sim_valid_barcode_scanned(3);
            }
            else if ( 0 == strncmp( press_button_cast, cmd, strlen(press_button_cast) ) )
            {
                reply = press_button_cast;
                sim_cast_button_pressed();
            }
            else if ( 0 == strncmp( pres_button_spoil, cmd, strlen(pres_button_spoil) ) )
            {
                reply = pres_button_spoil;
                sim_spoil_button_pressed();
            }
            else {
                reply = unknown_cmd;
            }

            snprintf( pcOutputBuffer, uxBufferLength, "%s\n", reply);
            return strlen( pcOutputBuffer );
        }
#endif // SIMULATION
        break;
    case ECMD_PATCH:
        // could be "/poke/address/length" with body having attack bytes
        if ( 0 == strncmp( "/poke/", pcURLData, 6 ) ) {
            strcpy( pxClient->pxParent->pcContentsType, "text/plain" );

            loadRequestData(pcURLData + 6, pxClient->pcRestData);
            
            int memAddress = getNextNumberFromURL();
            int writeLength = getNextNumberFromURL();
            uint8_t *mem = addressToUInt8Ptr( memAddress );

            if ( malware_region_start <= mem &&
                 0 < writeLength &&
                 mem + writeLength <= malware_region_end )
            {
                memcpy( mem, getHttpBody(), writeLength ); /* evil! */
                snprintf( pcOutputBuffer, uxBufferLength,
                          "Wrote %d bytes to %p for 'ya!\n", writeLength, mem );
            }
            else
            {
                snprintf( pcOutputBuffer, uxBufferLength,
                          "Couldn't write that amount of data to that address!\n");
            }

            return strlen( pcOutputBuffer );
        }
        break;
    default:
        break;
    }

    /* if there's an error */
    strcpy( pxClient->pxParent->pcContentsType, "text/plain" );
    snprintf( pcOutputBuffer, uxBufferLength, "Sorry, don't understand that.\n" );
    return strlen( pcOutputBuffer );
}

static TaskHandle_t xWebServerTaskHandle = NULL; /* written by xTaskCreate() */

static void prvWebServerTask( void *pvParameters )
{
    TCPServer_t *pxTCPServer = NULL;
    const TickType_t xInitialBlockTime = pdMS_TO_TICKS( 5000UL );

    /* A structure that defines the servers to be created.  Which servers are
       included in the structure depends on the mainCREATE_HTTP_SERVER and
       mainCREATE_FTP_SERVER settings at the top of this file. */
    static const struct xSERVER_CONFIG xServerConfiguration[] =
        {
            /* Server type,     port number,    backlog,    root dir. */
            { eSERVER_HTTP,     80,             12,         "" },
        };


    /* Remove compiler warning about unused parameter. */
    ( void ) pvParameters;

    FreeRTOS_debug_printf(("prvWebServerTask: Making TCP server\r\n"));

    /* Wait until the network is up before creating the servers.  The
       notification is given from the network event hook. Currently
       disabled because we're doing the relevant logic in sbb_tcp.c */
    /* ulTaskNotifyTake( pdTRUE, portMAX_DELAY ); */

    /* Create the servers defined by the xServerConfiguration array above. */
    pxTCPServer = FreeRTOS_CreateTCPServer( xServerConfiguration, sizeof( xServerConfiguration ) / sizeof( xServerConfiguration[ 0 ] ) );
    configASSERT( pxTCPServer );

    for ( ;; )
    {
        /* Run the HTTP and/or FTP servers, as configured above. */
        FreeRTOS_TCPServerWork( pxTCPServer, xInitialBlockTime );
    }
}

#define mainTCP_SERVER_STACK_SIZE               ( configMINIMAL_STACK_SIZE * 8 )

static UBaseType_t savedPriority = 0;

void peekPokeServerTaskPriority( UBaseType_t uxPriority )
{
    savedPriority = uxPriority;
}

static int alreadyCreated = 0;

void peekPokeServerTaskCreate( void )
{
    if ( !alreadyCreated )  
    {
        alreadyCreated = 1;
        xTaskCreate( prvWebServerTask, "WebServerTask", mainTCP_SERVER_STACK_SIZE, NULL, savedPriority, &xWebServerTaskHandle );
    }
}
