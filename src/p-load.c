/* p-load: Pololu USB Bootloader Utility.
 * This is the main source file.
 */

// TODO: consider adding a --pause-on-error option for IDEs like MicroCode Studio
// that let the output window close right after the program is done running

// Remove some warnings about safety in Windows.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

// For Debug builds in Windows, enable memory leak tracking.
#if defined(_MSC_VER) && defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

#include "message.h"
#include "version.h"
#include "arg_reader.h"
#include "actions.h"

#include "intel_hex.h"
#include "ploader.h"
#include "usb_system.h"

static const char help[] =
    "p-load: Pololu USB Bootloader Utility\n"
    "Version " VERSION "\n"
    "Usage: p-load OPTIONS\n"
    "\n"
    "Options available:\n"
    "  -d SERIALNUMBER             Specifies the serial number of the bootloader.\n"
    "  --list                      Lists bootloaders connected to computer.\n"
    "  --list-supported            Lists all types of bootloaders supported.\n"
    "  --wait                      Waits up to 10 seconds for bootloader to appear.\n"
    "  -w HEXFILE                  Writes to flash and EEPROM, then restarts.\n"
    "  --write HEXFILE             Writes to flash and EEPROM.\n"
    "  --write-flash HEXFILE       Writes to flash.\n"
    "  --write-eeprom HEXFILE      Writes to EEPROM.\n"
    "  --erase                     Erases flash and EEPROM.\n"
    "  --erase-flash               Erases flash.\n"
    "  --erase-eeprom              Erases EEPROM.\n"
    "  --read HEXFILE              Reads flash and EEPROM and saves to file.\n"
    "  --read-flash HEXFILE        Reads flash and saves to file.\n"
    "  --read-eeprom HEXFILE       Reads EEPROM and saves to file.\n"
    "  --restart                   Restarts the device so it can run the new code.\n"
    "\n"
    "HEXFILE is the name of the .HEX file to be used.\n"
    "\n"
    "Example: p-load -w app.hex\n"
    "Example: p-load -d 12345678 --wait --write-flash app.hex --restart\n"
    "Example: p-load --erase\n"
    "\n";

// The serial number of the bootloader the user wants to connect to,
// or NULL if none was specified.
const char * desiredSerialNumber = NULL;

bool waitForBootloaderFlag = false;

// True if we will restart the bootloader at the end.
bool restartBootloaderFlag = false;

// List of connected bootloaders
ploaderList * bootloaderList = NULL;

// Handle to the bootloader we are connected to.
ploaderHandle * bootloaderHandle = NULL;

ExitCode listSupportedBootloaders(void * data)
{
    printf("Supported bootloaders:\n");
    for (const ploaderProperties * prop = ploaderTable; prop->name; prop++)
    {
        printf("%s\n", prop->name);
    }
    return EXIT_SUCCESS;
}

ExitCode bootloaderListRequire()
{
    if (bootloaderList != NULL)
    {
        return 0;
    }

    PLOADER_RESULT result = ploaderListCreate(&bootloaderList);
    if (result){ return result; }

    if (desiredSerialNumber != NULL)
    {
        result = ploaderListFilterBySerialNumber(bootloaderList, desiredSerialNumber);
        if (result)
        {
            ploaderListFree(bootloaderList);
            bootloaderList = NULL;
            return ERROR_OPERATION_FAILED;
        }
    }

    return 0;
}

ExitCode bootloaderNotFoundError()
{
    if (desiredSerialNumber)
    {
        error("No bootloader found with serial number '%s'.", desiredSerialNumber);
    }
    else
    {
        error("No bootloader found.");
    }
    return ERROR_BOOTLOADER_NOT_FOUND;
}

ExitCode bootloaderNotFoundInfo()
{
    if (desiredSerialNumber)
    {
        info("No bootloader found with serial number '%s'.", desiredSerialNumber);
    }
    else
    {
        info("No bootloader found.");
    }
    return ERROR_BOOTLOADER_NOT_FOUND;
}

int printBootloaderInfo()
{
    ploaderInfo * pinfo;
    PLOADER_RESULT result = ploaderCreateInfo(bootloaderHandle, &pinfo);
    if (result){ return ERROR_OPERATION_FAILED; }

    info("Bootloader:    %s", pinfo->name);
    info("Serial number: %s", pinfo->serialNumber);

    ploaderInfoFree(pinfo);
    return 0;
}

ExitCode bootloaderHandleRequire()
{
    if (bootloaderHandle != NULL)
    {
        return 0;
    }

    PLOADER_RESULT result;

    ExitCode exitCode = bootloaderListRequire();
    if (exitCode) { return exitCode; }

    uint32_t size = ploaderListSize(bootloaderList);

    if (size == 0)
    {
        return bootloaderNotFoundError();
    }

    if (size > 1)
    {
        error("There are multiple qualifying bootloaders connected to this computer.\n"
            "Use the -d option to specify which bootloader you want to use, or disconnect\n"
            "the others.");
        return ERROR_OPERATION_FAILED;
    }

    result = ploaderOpenFromList(bootloaderList, 0, &bootloaderHandle);
    if (result)
    {
        return ERROR_OPERATION_FAILED;
    }

    printBootloaderInfo();

    return 0;
}

ExitCode waitForBootloaderIfNeeded()
{
    if (!waitForBootloaderFlag) { return 0; }

    time_t waitStartTime = time(NULL);

    while(1)
    {
        ExitCode exitCode = bootloaderListRequire();
        if (exitCode) { return exitCode; }

        if (ploaderListSize(bootloaderList) > 0)
        {
            return EXIT_SUCCESS;
        }

        ploaderListFree(bootloaderList);
        bootloaderList = NULL;

        if (difftime(time(NULL), waitStartTime) > 10)
        {
            return bootloaderNotFoundError();
        }

		// Sleep so that we don't take up 100% CPU time.
#ifdef _WIN32
		Sleep(100);     // 100 ms
#else
        usleep(100000); // 100 ms
#endif
    }
}

// Returns a human-readable string representing what state the bootloader
// is in, for use in the "list" action.
const char * getStatus(ploaderList * list, uint32_t index)
{
    ploaderHandle * handle;
    PLOADER_RESULT result;
    result = ploaderOpenFromList(list, index, &handle);
    if (result)
    {
        fprintf(stderr, "Warning: Unable to connect to bootloader.\n");
        return "?";
    }

    bool appValid;
    result = ploaderCheckApplication(handle, &appValid);
    if (result)
    {
        fprintf(stderr, "Warning: Unable to check application.\n");
        ploaderClose(handle);
        return "?";
    }

    ploaderClose(handle);
    return appValid ? "App present" : "No app present";
}

// Prints a list of bootloaders connected to the computer.
// Returns a process exit code or 0.
ExitCode listConnectedBootloaders(void * data)
{
    // Since we can't have two handles open at once in Windows, close the handle
    // if we already opened one.
    ploaderClose(bootloaderHandle);
    bootloaderHandle = NULL;

    unsigned int i;
    PLOADER_RESULT result;
    uint32_t bootloaderCount;

    result = bootloaderListRequire();
    if (result){ return ERROR_OPERATION_FAILED; }

    bootloaderCount = ploaderListSize(bootloaderList);

    for (i = 0; i < bootloaderCount; i++)
    {
        ploaderInfo * usbInfo;
        result = ploaderListCreateInfo(bootloaderList, i, &usbInfo);
        if (result)
        {
            return ERROR_OPERATION_FAILED;
        }

        const char * status = getStatus(bootloaderList, i);

        printf("%-11s  %-40s %-15s\n", usbInfo->serialNumber, usbInfo->name, status);

        ploaderInfoFree(usbInfo);
    }

    if (bootloaderCount == 0)
    {
        // This behavior lets people use "p-load list" to easily detect from a
        // shell script whether or not a certain bootloader is connected.
        return bootloaderNotFoundInfo();
    }

    return 0;
}

ExitCode restartBootloaderIfNeeded()
{
    if (!restartBootloaderFlag) { return 0; }

    ExitCode exitCode = bootloaderHandleRequire();
    if (exitCode) { return exitCode; }

    PLOADER_RESULT result = ploaderRestartDevice(bootloaderHandle);
    if (result) { return ERROR_OPERATION_FAILED;  }

    return 0;
}

typedef struct HexFileInput
{
    const char * fileName;
    uint8_t * flash;
    uint8_t * eeprom;
} HexFileInput;

ExitCode allocateHexFileInput(void ** data)
{
    assert(data != NULL);
    *data = malloc(sizeof(HexFileInput));
    if (*data == NULL)
    {
        error("Failed to allocate memory for image struct.");
        return ERROR_OPERATION_FAILED;
    }
    memset(*data, 0, sizeof(HexFileInput));
    return 0;
}

void freeHexFileInput(void * x)
{
    HexFileInput * data = x;
    if (data != NULL)
    {
        free(data->flash);
        free(data->eeprom);
        free(data);
    }
}

ExitCode parseHexFileInputArg(void * x)
{
    HexFileInput * data = x;
    assert(data != NULL);

    const char * arg = argReaderNext();
    if (arg == NULL)
    {
        error("Expected a filename after %s.", argReaderLast());
        return ERROR_BAD_ARGS;
    }
    data->fileName = arg;
    return 0;
}

ExitCode readHexFile(void * x)
{
    HexFileInput * data = x;

    assert(data != NULL);
    assert(data->fileName != NULL);
    assert(data->flash == NULL);
    assert(data->eeprom == NULL);

    // Get information about the bootloader's memory regions.
    ExitCode exitCode = bootloaderHandleRequire();
    if (exitCode) { return exitCode; }
    ploaderInfo * pInfo;
    PLOADER_RESULT ploaderResult = ploaderCreateInfo(bootloaderHandle, &pInfo);
    if (ploaderResult)
    {
        return ERROR_OPERATION_FAILED;
    }
    const uint32_t appSize = pInfo->appSize;
    const uint32_t appAddress = pInfo->appAddress;
    const uint32_t eepromSize = pInfo->eepromSize;
    const uint32_t eepromAddressHexFile = pInfo->eepromAddressHexFile;
    ploaderInfoFree(pInfo);

    // Make a buffer for holding flash.
    data->flash = malloc(appSize);
    data->eeprom = malloc(eepromSize);
    if (data->flash == NULL || data->eeprom == NULL)
    {
        error("Failed to allocate memory for image.");
        return ERROR_OPERATION_FAILED;
    }
    memset(data->flash, 0xFF, appSize);
    memset(data->eeprom, 0xFF, eepromSize);

    // Read in the HEX file.
    FILE * file = fopen(data->fileName, "r");
    if (file == NULL)
    {
        error("%s: %s", data->fileName, strerror(errno));
        return ERROR_OPERATION_FAILED;
    }

    ihxMemory memories[3] = {
        { data->flash, appAddress, appAddress + appSize},
        { data->eeprom, eepromAddressHexFile, eepromAddressHexFile + eepromSize },
        { NULL },
    };

    IHX_RESULT ihxResult = ihxRead(file, data->fileName, NULL, memories);
    if (ihxResult)
    {
        fclose(file);
        return ERROR_OPERATION_FAILED;
    }
    fclose(file);

    return 0;
}

// Allocates memory for the memories in a HexFileInput struct and sets them
// to just be all 0xFF.
ExitCode clearHexFileInput(void * x)
{
    HexFileInput * data = x;

    assert(data != NULL);
    assert(data->fileName == NULL);
    assert(data->flash == NULL);
    assert(data->eeprom == NULL);

    // Get information about the bootloader's memory regions.
    ExitCode exitCode = bootloaderHandleRequire();
    if (exitCode) { return exitCode; }
    ploaderInfo * pInfo;
    PLOADER_RESULT ploaderResult = ploaderCreateInfo(bootloaderHandle, &pInfo);
    if (ploaderResult)
    {
        return ERROR_OPERATION_FAILED;
    }
    const uint32_t appSize = pInfo->appSize;
    const uint32_t eepromSize = pInfo->eepromSize;
    ploaderInfoFree(pInfo);

    // Make a buffer for holding flash.
    data->flash = malloc(appSize);
    data->eeprom = malloc(eepromSize);
    if (data->flash == NULL || data->eeprom == NULL)
    {
        error("Failed to allocate memory for image.");
        return ERROR_OPERATION_FAILED;
    }
    memset(data->flash, 0xFF, appSize);
    memset(data->eeprom, 0xFF, eepromSize);

    return 0;
}

ExitCode writeFlashAndEeprom(void * x)
{
    HexFileInput * data = x;

    assert(data != NULL);
    assert(data->flash != NULL);
    assert(data->eeprom != NULL);

    ExitCode exitCode = bootloaderHandleRequire();
    if (exitCode) { return exitCode; }

    // TODO: do it in a nicer way where we erase flash, write EEPROM, then write flash
    // so that there is no chance of the application running before EEPROM is ready

    PLOADER_RESULT ploaderResult = ploaderWriteFlash(bootloaderHandle,
        data->flash, &statusCallback);
    if (ploaderResult)
    {
        return ERROR_OPERATION_FAILED;
    }

    ploaderResult = ploaderWriteEeprom(bootloaderHandle, data->eeprom, &statusCallback);
    if (ploaderResult)
    {
        return ERROR_OPERATION_FAILED;
    }
    return 0;
}

ExitCode writeFlash(void * x)
{
    HexFileInput * data = x;

    assert(data != NULL);
    assert(data->flash != NULL);

    ExitCode exitCode = bootloaderHandleRequire();
    if (exitCode) { return exitCode; }

    PLOADER_RESULT ploaderResult = ploaderWriteFlash(bootloaderHandle,
        data->flash, &statusCallback);
    if (ploaderResult)
    {
        return ERROR_OPERATION_FAILED;
    }
    return 0;
}

ExitCode writeEeprom(void * x)
{
    HexFileInput * data = x;

    assert(data != NULL);
    assert(data->flash != NULL);
    assert(data->eeprom != NULL);

    ExitCode exitCode = bootloaderHandleRequire();
    if (exitCode) { return exitCode; }

    PLOADER_RESULT ploaderResult = ploaderWriteEeprom(bootloaderHandle,
        data->eeprom, &statusCallback);
    if (ploaderResult)
    {
        return ERROR_OPERATION_FAILED;
    }
    return 0;
}

typedef struct HexFileOutput
{
    const char * fileName;
    FILE * file;
} HexFileOutput;

ExitCode allocateHexFileOutput(void ** data)
{
    assert(data != NULL);
    *data = malloc(sizeof(HexFileOutput));
    if (*data == NULL)
    {
        error("Failed to allocate memory for image struct.");
        return ERROR_OPERATION_FAILED;
    }
    memset(*data, 0, sizeof(HexFileOutput));
    return 0;
}

void freeHexFileOutput(void * x)
{
    HexFileOutput * data = x;
    if (data != NULL)
    {
        if (data->file != NULL)
        {
            fclose(data->file);
        }
        free(data);
    }
}

ExitCode parseHexFileOutputArg(void * x)
{
    HexFileOutput * data = x;
    assert(data != NULL);

    const char * arg = argReaderNext();
    if (arg == NULL)
    {
        error("Expected a filename after %s.", argReaderLast());
        return ERROR_BAD_ARGS;
    }
    data->fileName = arg;
    return 0;
}

ExitCode prepareHexFileOutput(void * x)
{
    HexFileOutput * data = x;
    assert(data != NULL);

    data->file = fopen(data->fileName, "w");
    if (data->file == NULL)
    {
        error("%s: %s", data->fileName, strerror(errno));
        return ERROR_OPERATION_FAILED;
    }
    return 0;
}

ExitCode readMemories(HexFileOutput * data, bool readFlash, bool readEeprom)
{
    ExitCode exitCode = bootloaderHandleRequire();
    if (exitCode) { return exitCode; }

    // Get bootloader memory region addresses.
    ploaderInfo * pInfo;
    PLOADER_RESULT ploaderResult = ploaderCreateInfo(bootloaderHandle, &pInfo);
    if (ploaderResult)
    {
        return ERROR_OPERATION_FAILED;
    }
    const uint32_t appAddress = pInfo->appAddress;
    const uint32_t appSize = pInfo->appSize;
    const uint32_t eepromAddressHexFile = pInfo->eepromAddressHexFile;
    const uint32_t eepromSize = pInfo->eepromSize;
    ploaderInfoFree(pInfo);

    ihxMemory ihxMemories[3];
    int memoryCount = 0;

    // Read from the bootloader's flash if needed.
    uint8_t * flash = NULL;
    if (readFlash)
    {
        flash = malloc(appSize);
        if (flash == NULL)
        {
            error("Failed to allocate memory for flash image.");
            return ERROR_OPERATION_FAILED;
        }

        ploaderResult = ploaderReadFlash(bootloaderHandle, flash, &statusCallback);
        if (ploaderResult)
        {
            free(flash);
            return ERROR_OPERATION_FAILED;
        }

        ihxMemories[memoryCount++] = (ihxMemory) { flash, appAddress, appAddress + appSize };
    }

    // Read from the bootloader's EEPROM if needed.
    uint8_t * eeprom = NULL;
    if (readEeprom)
    {
        eeprom = malloc(eepromSize);
        if (eeprom == NULL)
        {
            error("Failed to allocate memory for EEPROM image.");
            free(flash);
            return ERROR_OPERATION_FAILED;
        }

        ploaderResult = ploaderReadEeprom(bootloaderHandle, eeprom, &statusCallback);
        if (ploaderResult)
        {
            free(eeprom);
            free(flash);
            return ERROR_OPERATION_FAILED;
        }

        ihxMemories[memoryCount++] = (ihxMemory) { eeprom,
            eepromAddressHexFile, eepromAddressHexFile + eepromSize };
    }

    // Finish the ihxMemories array by adding the null terminator
    ihxMemories[memoryCount] = (ihxMemory) { NULL };

    // Write to the file.
    IHX_RESULT ihxResult = ihxWrite(data->file, ihxMemories);
    free(eeprom);
    free(flash);
    if (ihxResult)
    {
        return ERROR_OPERATION_FAILED;
    }

    return 0;
}

ExitCode readFlashAndEeprom(void * x)
{
    HexFileOutput * data = x;
    return readMemories(data, true, true);
}

ExitCode readFlash(void * x)
{
    HexFileOutput * data = x;
    return readMemories(data, true, false);
}

ExitCode readEeprom(void * x)
{
    HexFileOutput * data = x;
    return readMemories(data, false, true);
}

const actionType actList = {
    .execute = &listConnectedBootloaders,
};

const actionType actListSupported = {
    .execute = &listSupportedBootloaders,
};

const actionType actWriteFlashAndEeprom = {
    .allocate = &allocateHexFileInput,
    .parse = &parseHexFileInputArg,
    .prepare = &readHexFile,
    .execute = &writeFlashAndEeprom,
    .freeFunc = &freeHexFileInput,
};

const actionType actWriteFlash = {
    .allocate = &allocateHexFileInput,
    .parse = &parseHexFileInputArg,
    .prepare = &readHexFile,
    .execute = &writeFlash,
    .freeFunc = &freeHexFileInput,
};

const actionType actWriteEeprom = {
    .allocate = &allocateHexFileInput,
    .parse = &parseHexFileInputArg,
    .prepare = &readHexFile,
    .execute = &writeEeprom,
    .freeFunc = &freeHexFileInput,
};

const actionType actEraseFlashAndEeprom = {
    .allocate = &allocateHexFileInput,
    .prepare = &clearHexFileInput,
    .execute = &writeFlashAndEeprom,
    .freeFunc = &freeHexFileInput,
};

const actionType actEraseFlash = {
    .allocate = &allocateHexFileInput,
    .prepare = &clearHexFileInput,
    .execute = &writeFlash,
    .freeFunc = &freeHexFileInput,
};

const actionType actEraseEeprom = {
    .allocate = &allocateHexFileInput,
    .prepare = &clearHexFileInput,
    .execute = &writeEeprom,
    .freeFunc = &freeHexFileInput,
};

const actionType actReadFlashAndEeprom = {
    .allocate = &allocateHexFileOutput,
    .parse = &parseHexFileOutputArg,
    .prepare = &prepareHexFileOutput,
    .execute = &readFlashAndEeprom,
    .freeFunc = &freeHexFileOutput,
};

const actionType actReadFlash = {
    .allocate = &allocateHexFileOutput,
    .parse = &parseHexFileOutputArg,
    .prepare = &prepareHexFileOutput,
    .execute = &readFlash,
    .freeFunc = &freeHexFileOutput,
};

const actionType actReadEeprom = {
    .allocate = &allocateHexFileOutput,
    .parse = &parseHexFileOutputArg,
    .prepare = &prepareHexFileOutput,
    .execute = &readEeprom,
    .freeFunc = &freeHexFileOutput,
};

ExitCode parseArgs()
{
    const char * arg;
    while (1)
    {
        arg = argReaderNext();

        if (arg == NULL)
        {
            break;  // Done reading arguments.
        }

        int exitCode = 0;

        if (strcmp(arg, "-d") == 0)
        {
            if (desiredSerialNumber != NULL)
            {
                error("Serial number can only be specified once.");
                return ERROR_BAD_ARGS;
            }
            desiredSerialNumber = argReaderNext();
            if (desiredSerialNumber == NULL)
            {
                error("Expected a serial number after %s.", arg);
                return ERROR_BAD_ARGS;
            }
        }
        else if (strcmp(arg, "--list") == 0)
        {
            exitCode = actionsAdd(&actList);
        }
        else if (strcmp(arg, "--list-supported") == 0)
        {
            exitCode = actionsAdd(&actListSupported);
        }
        else if (strcmp(arg, "--wait") == 0)
        {
            waitForBootloaderFlag = true;
        }
        else if (strcmp(arg, "-w") == 0)
        {
            exitCode = actionsAdd(&actWriteFlashAndEeprom);
            restartBootloaderFlag = true;
        }
        else if (strcmp(arg, "--write") == 0)
        {
            exitCode = actionsAdd(&actWriteFlashAndEeprom);
        }
        else if (strcmp(arg, "--write-flash") == 0)
        {
            exitCode = actionsAdd(&actWriteFlash);
        }
        else if (strcmp(arg, "--write-eeprom") == 0)
        {
            exitCode = actionsAdd(&actWriteEeprom);
        }
        else if (strcmp(arg, "--erase") == 0)
        {
            exitCode = actionsAdd(&actEraseFlashAndEeprom);
        }
        else if (strcmp(arg, "--erase-flash") == 0)
        {
            exitCode = actionsAdd(&actEraseFlash);
        }
        else if (strcmp(arg, "--erase-eeprom") == 0)
        {
            exitCode = actionsAdd(&actEraseEeprom);
        }
        else if (strcmp(arg, "--read") == 0)
        {
            exitCode = actionsAdd(&actReadFlashAndEeprom);
        }
        else if (strcmp(arg, "--read-flash") == 0)
        {
            exitCode = actionsAdd(&actReadFlash);
        }
        else if (strcmp(arg, "--read-eeprom") == 0)
        {
            exitCode = actionsAdd(&actReadEeprom);
        }
        else if (strcmp(arg, "--restart") == 0)
        {
            restartBootloaderFlag = true;
        }
        else
        {
            error("Unknown option: %s", arg);
            return ERROR_BAD_ARGS;
        }

        if (exitCode)
        {
            return exitCode;
        }
    }

    return 0;
}

ExitCode run()
{
    int exitCode;

    exitCode = parseArgs();
    if (exitCode) { return exitCode; }

    exitCode = waitForBootloaderIfNeeded();
    if (exitCode) { return exitCode; }

    exitCode = actionsPrepare();
    if (exitCode) { return exitCode; }

    exitCode = actionsExecute();
    if (exitCode) { return exitCode; }

    exitCode = restartBootloaderIfNeeded();
    if (exitCode) { return exitCode; }

    return 0;
}

// main: This is the first function to run when this utility is started.
int main(int argc, char ** argv)
{
    usbSetVerbosity(3);

#if defined(_MSC_VER) && defined(_DEBUG)
    // For a Debug build in Windows, send a report of memory leaks to
    // the Debug pane of the Output window in Visual Studio.
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    if (argc <= 1)
    {
        printf(help);
        return ERROR_BAD_ARGS;
    }

    argReaderInit(argc, argv);

    int exitCode = run();

    actionsFree();
    ploaderClose(bootloaderHandle);
    ploaderListFree(bootloaderList);

    return exitCode;
}
