/*
 *  Released under "The GNU General Public License (GPL-2.0)"
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include "IntelHDA.h"

IORegistryEntry* IntelHDA::getHDADriver(IORegistryEntry* registryEntry)
{
    IORegistryEntry* entry = registryEntry->getChildEntry(gIOServicePlane);
    
    if (entry != NULL)
    {
        if (strcasecmp(entry->getName(), "AppleHDADriver") == 0)
            return entry;
        
        return getHDADriver(entry);
    }
    
    return NULL;
}

IntelHDA::IntelHDA(IOService *service, HDACommandMode commandMode)
{
    mCommandMode = commandMode;
    mDevice = OSDynamicCast(IOPCIDevice, service);
}

IntelHDA::~IntelHDA()
{
    OSSafeReleaseNULL(mMemoryMap);
}

bool IntelHDA::initialize()
{
    IOLog("CodecCommander::IntelHDA\n");
    
    if (mDevice == NULL || mDevice->getDeviceMemoryCount() == 0)
        return false;
    
    mDevice->setMemoryEnable(true);
    
    mDeviceMemory = mDevice->getDeviceMemoryWithIndex(0);
    
    if (mDeviceMemory == NULL)
    {
        IOLog("IntelHDA: Failed to access device memory.\n");
        return false;
    }
    
    DEBUG_LOG("IntelHDA: Device memory @ 0x%08llx, size 0x%08llx\n", mDeviceMemory->getPhysicalAddress(), mDeviceMemory->getLength());
        
    mMemoryMap = mDeviceMemory->map();

    if (mMemoryMap == NULL)
    {
        IOLog("IntelHDA: Failed to map device memory.\n");
        return false;
    }
    
    DEBUG_LOG("IntelHDA: Memory mapped at @ 0x%08llx\n", mMemoryMap->getVirtualAddress());
        
    mRegMap = (pHDA_REG)mMemoryMap->getVirtualAddress();
    
    char devicePath[1024];
    int pathLen = sizeof(devicePath);
    bzero(devicePath, sizeof(devicePath));
    
    uint32_t deviceInfo = mDevice->configRead32(0);
    
    if (mDevice->getPath(devicePath, &pathLen, gIOServicePlane))
        IOLog("IntelHDA: Evaluating device \"%s\" [%04x:%04x].\n",
                  devicePath,
                  deviceInfo >> 16,
                  deviceInfo & 0x0000FFFF);
        
    if (mRegMap->VMAJ == 1 &&
        mRegMap->VMIN == 0 &&
        this->getVendorId() != 0xFFFF)
    {
        IORegistryEntry* hdaDriver = getHDADriver(mDevice);
        
        if (hdaDriver != NULL)
        {
            pathLen = sizeof(devicePath);
            bzero(devicePath, sizeof(devicePath));
            
            if (hdaDriver->getPath(devicePath, &pathLen, gIOServicePlane))
                DEBUG_LOG("IntelHDA: Located HDA driver at \"%s\".\n", devicePath);
            
            IOLog("....Output Streams:\t%d\n", mRegMap->GCAP_OSS);
            IOLog("....Input Streams:\t%d\n", mRegMap->GCAP_ISS);
            IOLog("....Bidi Streams:\t%d\n", mRegMap->GCAP_BSS);
            IOLog("....Serial Data:\t%d\n", mRegMap->GCAP_NSDO);
            IOLog("....x64 Support:\t%d\n", mRegMap->GCAP_64OK);
            IOLog("....Codec Version:\t%d.%d\n", mRegMap->VMAJ, mRegMap->VMIN);
            IOLog("....Vendor Id:\t\t0x%04x\n", this->getVendorId());
            IOLog("....Device Id:\t\t0x%04x\n", this->getDeviceId());

            return true;
        }
    }
    
    return false;
}

void IntelHDA::SetCodecAddress(UInt8 codecAddress)
{
    mCodecAddress = codecAddress;
}

IORegistryEntry* IntelHDA::getHDADriver()
{
    return IntelHDA::getHDADriver(mDevice);
}

UInt16 IntelHDA::getVendorId()
{
    if (mVendor == -1)
        mVendor = this->sendCommand(0, HDA_VERB_GET_PARAM, HDA_PARM_VENDOR);
    
    return mVendor >> 16;
}

UInt16 IntelHDA::getDeviceId()
{
    if (mVendor == -1)
        mVendor = this->sendCommand(0, HDA_VERB_GET_PARAM, HDA_PARM_VENDOR);
    
    return mVendor & 0xFFFF;
}

UInt8 IntelHDA::getTotalNodes()
{
    if (mNodes == -1)
        mNodes = this->sendCommand(1, HDA_VERB_GET_PARAM, HDA_PARM_NODECOUNT);
    
    return ((mNodes & 0x0000FF) >>  0) + 1;
}

UInt8 IntelHDA::getStartingNode()
{
    if (mNodes == -1)
        mNodes = this->sendCommand(1, HDA_VERB_GET_PARAM, HDA_PARM_NODECOUNT);
    
    return (mNodes & 0xFF0000) >> 16;
}

UInt32 IntelHDA::sendCommand(UInt8 nodeId, UInt16 verb, UInt8 payload)
{
    DEBUG_LOG("IntelHDA::SendCommand: node 0x%02x, verb 0x%06x, payload 0x%02x.\n", nodeId, verb, payload);
    return this->sendCommand((nodeId & 0xFF) << 20 | (verb & 0xFFF) << 8 | payload);
}

UInt32 IntelHDA::sendCommand(UInt8 nodeId, UInt8 verb, UInt16 payload)
{
    DEBUG_LOG("IntelHDA::SendCommand: node 0x%02x, verb 0x%02x, payload 0x%04x.\n", nodeId, verb, payload);
    return this->sendCommand((nodeId & 0xFF) << 20 | (verb & 0xF) << 16 | payload);
}

UInt32 IntelHDA::sendCommand(UInt32 command)
{
    UInt32 fullCommand = (mCodecAddress & 0xF) << 28 | (command & 0x0FFFFFFF);
    
    if (mDeviceMemory == NULL)
        return -1;
    
    DEBUG_LOG("IntelHDA::SendCommand: (w) --> 0x%08x\n", fullCommand);
  
    UInt32 response = -1;
    
    switch (mCommandMode)
    {
        case PIO:
            response = this->executePIO(fullCommand);
            break;
        case DMA:
            IOLog("IntelHDA: Unsupported command mode DMA requested.\n");
            response = -1;
            break;
        default:
            response = -1;
            break;
    }
    
    DEBUG_LOG("IntelHDA::SendCommand: (r) <-- 0x%08x\n", response);
    
    return response;
}

UInt32 IntelHDA::executePIO(UInt32 command)
{
    UInt16 status;
    
    status = 0x1; // Busy status
    
    for (int i = 0; i < 1000; i++)
    {
        status = mRegMap->ICS;
        
        if (!HDA_ICS_IS_BUSY(status))
            break;
        
        ::IODelay(100);
    }
    
    // HDA controller was not ready to receive PIO commands
    if (HDA_ICS_IS_BUSY(status))
    {
        DEBUG_LOG("IntelHDA::ExecutePIO timed out waiting for ICS readiness.\n");
        return -1;
    }
    
    //DEBUG_LOG("IntelHDA::ExecutePIO ICB bit clear.\n");
    
    // Queue the verb for the HDA controller
    mRegMap->ICW = command;
    
    status = 0x1; // Busy status
    mRegMap->ICS = status;
    
    //DEBUG_LOG("IntelHDA::ExecutePIO Wrote verb and set ICB bit.\n");
    
    // Wait for HDA controller to return with a response
    for (int i = 0; i < 1000; i++)
    {
        status = mRegMap->ICS;
        
        if (!HDA_ICS_IS_BUSY(status))
            break;
        
        ::IODelay(100);
    }
    
    // Store the result validity while IRV is cleared
    bool validResult = HDA_ICS_IS_VALID(status);
    
    UInt32 response;
    
    if (validResult)
        response = mRegMap->IRR;
    
    // Reset IRV
    status = 0x02; // Valid, Non-busy status
    mRegMap->ICS = status;
    
    if (!validResult)
    {
        DEBUG_LOG("IntelHDA::ExecutePIO Invalid result received.\n");
        return -1;
    }
    
    return response;
}