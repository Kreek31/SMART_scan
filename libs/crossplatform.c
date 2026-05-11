#include "crossplatform.h"
#include <string.h>
#include <stdio.h>

void loadSataSMARTAttributes(SmartData* smartData, unsigned char data_buffer[SMART_DATA_SIZE_BYTES]){
    memcpy(&smartData->smartVersion, &data_buffer[0], 2);
    size_t count = 0;

    for (size_t smartIter = SATA_SMART_BEGIN_ATTRIBUTES; smartIter < SATA_SMART_END_ATTRIBUTES; smartIter += SATA_SMART_ATTRIBUTE_SIZE)
    {
        unsigned char id = data_buffer[smartIter + 0];

        if (!(id > 0 && id < 255)){
            continue;
        }

        SataSMARTAttribute* currentAttribute = &smartData->attributes[id].attribute;
        currentAttribute->id = data_buffer[smartIter + 0];

        currentAttribute->flags[0]   = data_buffer[smartIter + 1];
        currentAttribute->flags[1]   = data_buffer[smartIter + 2];
        currentAttribute->normalized = data_buffer[smartIter + 3];
        currentAttribute->worst      = data_buffer[smartIter + 4];
        currentAttribute->raw[0]     = data_buffer[smartIter + 5];
        currentAttribute->raw[1]     = data_buffer[smartIter + 6];
        currentAttribute->raw[2]     = data_buffer[smartIter + 7];
        currentAttribute->raw[3]     = data_buffer[smartIter + 8];
        currentAttribute->raw[4]     = data_buffer[smartIter + 9];
        currentAttribute->raw[5]     = data_buffer[smartIter + 10];
        currentAttribute->reserved   = data_buffer[smartIter + 11];
    }
}



void loadSataSMARTThresholds(SmartData* smartData, unsigned char data_buffer[SMART_DATA_SIZE_BYTES]){
    memcpy(&smartData->smartVersion, &data_buffer[0], 2);
    size_t count = 0;

    for (size_t smartIter = SATA_SMART_BEGIN_ATTRIBUTES; smartIter < SATA_SMART_END_ATTRIBUTES; smartIter += SATA_SMART_ATTRIBUTE_SIZE)
    {
        unsigned char id = data_buffer[smartIter + 0];

        if (!(id > 0 && id < 255)){
            continue;
        }

        SataSMARTThreshold* currentThreshold = &smartData->attributes[id].threshold;
        currentThreshold->id = data_buffer[smartIter + 0];

        currentThreshold->threshold   = data_buffer[smartIter + 1];
        currentThreshold->reserved[0] = data_buffer[smartIter + 2];
        currentThreshold->reserved[1] = data_buffer[smartIter + 3];
        currentThreshold->reserved[2] = data_buffer[smartIter + 4];
        currentThreshold->reserved[3] = data_buffer[smartIter + 5];
        currentThreshold->reserved[4] = data_buffer[smartIter + 6];
        currentThreshold->reserved[5] = data_buffer[smartIter + 7];
        currentThreshold->reserved[6] = data_buffer[smartIter + 8];
        currentThreshold->reserved[7] = data_buffer[smartIter + 9];
        currentThreshold->reserved[8] = data_buffer[smartIter + 10];
        currentThreshold->reserved[9] = data_buffer[smartIter + 11];
    }
}