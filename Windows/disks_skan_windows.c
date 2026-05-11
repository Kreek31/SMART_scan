#include <stdio.h>
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <ntddstor.h>
#include <ntddscsi.h>
#include <stdbool.h>
#include <winioctl.h>
#include <assert.h>
#include <nvme.h>

#include "crossplatform.h"

//minimum Windows version: Windows 10 (IOCTL_STORAGE_PROTOCOL_COMMAND requironment)

//TODO: Проверить правильность чтения thresholds
//TODO: Перепроверить вызов IOCTL_ATA_PASS_THROUGH для получения статуса SMART для SATA
//TODO: Попробовать переписать для SATA вызов Smart Read Data с использованием IOCTL_ATA_PASS_THROUGH
//TODO: IOCTL_STORAGE_QUERY_PROPERTY - посмотреть все варианты возвращаемых байтов при успешном и неуспешном запросе и переписать структуры, которые принимают эти байты.
//TODO: Реализовать неточный поиск модели в файле. Для этого, как вариант, удалять символы из полученной строки до тех пор, пока не встретим первое совпадение.
//TODO: Реализовать получение значения SMART таблицы по байтам из справочника

#define SG_ATA_16 0x85
#define ATA_SMART_CMD 0xB0
#define NVME_ADMIN_GET_LOG_PAGE 0x02
#define SMART_READ_DATA 0xD0
#define SMART_RETURN_STATUS 0xB0
#define SMART_RETURN_STATUS_FEATURE 0xDA
#define SMART_CYL_LOW 0x4F
#define SMART_CYL_HIGH 0xC2
#define SMART_DATA_SIZE_DWORDS 128
#define SMART_LID 0x02

#define MAX_GROUPS 64
#define MAX_MODELS 16
#define MAX_STRING_LEN 1024

FILE * sata_dict;
int model_group_count = 0;


//Структура используется для объединения множества моделей дисков в одну группу
typedef struct {
    char *name;        // Название группы (например, "Seagate")
    char *models[MAX_MODELS];     // Список моделей в группе
    int model_count;
} Group;
Group model_groups[MAX_GROUPS];


//Структура объединяет в себе 'SCSI_PASS_THROUGH' с буфферами, которые используются в команде 'IOCTL_SCSI_PASS_THROUGH'
typedef struct SCSI_PASS_THROUGH_WITH_BUFFERS {
    SCSI_PASS_THROUGH spt;
    UCHAR SenseBuf[32];
    UCHAR DataBuf[SMART_DATA_SIZE_BYTES];
} SCSI_PASS_THROUGH_WITH_BUFFERS;


//Структура объединяет в себе 'STORAGE_PROTOCOL_COMMAND' с буфферами, отступ к которым указан в полях 'STORAGE_PROTOCOL_COMMAND'
//pragma pack используется для временного отключения выравнивания полей структуры из-за необходимости плотного расположения полей 'spc' и 'spcCommandField', второе из которых по сути является продолжением поля 'spc.Command'
#pragma pack(push, 1)
typedef struct STORAGE_PROTOCOL_COMMAND_WITH_BUFFERS{
    STORAGE_PROTOCOL_COMMAND spc;
    BYTE spcCommandField[64];
    UCHAR ErrorInfo[4096];
    UCHAR DataToDeviceBuffer[4096];
    UCHAR DataFromDeviceBuffer[4096];
} STORAGE_PROTOCOL_COMMAND_WITH_BUFFERS;
#pragma pack(pop)


//Эта функция избавляется от лишних пробелов в начале и конце строки '*str'.
//Состояние '*str' при этом меняется в соответствии с работой функции.
//Пример:
//char str[7] = "  abc   ";
//TrimString(str, strlen(str)+1) <- эквивалентно записи 'str = "abc"'.
void TrimString(char *str) {
    size_t size = strlen(str)+1;
    for (size_t i = size - 2; i >= 0 && isspace((unsigned char)str[i]); --i) {
        str[i] = '\0';//если строка неизменяемая, то получаем runtime error
    }

    char *start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != str) {
        size_t len = strlen(start);
        memmove(str, start, len + 1);
    }
}


//Эта функция считывает из предоставленного ini файла 'filename' модели дисков и их группы, а затем сохраняет полученные данные в массиве.
//В результате работы заполняется глобальный массив 'groups' и его счетчик
void LoadGroups(FILE * sata_dict) {
    if (sata_dict == NULL){
        fprintf(stderr, "Wrong filename in function 'LoadGroups'.\n");
        return;
    }
    fseek(sata_dict, 0, 0);

    char line[MAX_STRING_LEN];
    bool in_groups_section = false;
    while (fgets(line, sizeof(line), sata_dict)) {
        TrimString(line);
        if (line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            if (!in_groups_section){
                in_groups_section = (strncmp(line, "[Groups]", 8) == 0);
                continue;
            } else {
                break;
            }
        }
        if (!in_groups_section) continue;

        // Разбираем строку вида: Seagate=ST1000DM003,ST2000DM008
        char *equivalent = strchr(line, '=');
        if (!equivalent) continue;

        *equivalent = '\0';
        char *group_name = line;
        char *models_str = equivalent + 1;

        TrimString(group_name);
        TrimString(models_str);
        //printf("line:\"%s\"\n", group_name);

        if (model_group_count >= MAX_GROUPS) {
            fprintf(stderr, "Group count exceeded MAX_GROUPS value.");
            break;
        }

        Group *g = &model_groups[model_group_count++];
        g->name = malloc(strlen(group_name)+1);
        strncpy(g->name, group_name, strlen(group_name)+1);

        // Разбиваем список моделей по запятой и "
        char *token = strtok(models_str, ",\"");
        while (token && g->model_count < MAX_MODELS) {
            int token_length = strlen(token)+1;
            TrimString(token);
            g->models[g->model_count] = malloc(token_length);
            strncpy(g->models[g->model_count++], token, token_length);
            token = strtok(NULL, ",\"");
        }
    }
    fseek(sata_dict, 0, 0);
}


//Эта функция возвращает поле 'name' структуры 'Group', которая содержит указанную модель диска '*model'
//В данном варианте реализован поиск по наибольшему префиксу '*model', который имеется в списке 'Group' без учета регистра (заглавная/строчная буква)
char *FindGroupByModel(const char *model) {
    int target_model_strlen = strlen(model);
    int max_prefix_len = 0; //максимальная длинна названия модели в файле, которая является префиксом целевого названия модели
    int max_prefix_len_group = -1;
    int model_index = 0;
    for (int i = 0; i < model_group_count; i++) {
        for (int j = 0; j < model_groups[i].model_count; j++) {
            int current_prefix = 0;
            int infile_model_strlen = strlen(model_groups[i].models[j]);

            if (infile_model_strlen > target_model_strlen){
                continue;
            }

            for (int k = 0; k < infile_model_strlen; k++){
                if(model_groups[i].models[j][k] != model[k]){
                    current_prefix = 0;
                    break;
                }
                current_prefix++;
            }

            if (current_prefix > max_prefix_len){
                max_prefix_len_group = i;
                max_prefix_len = current_prefix;
                model_index = j;
            }
        }
    }

    if (max_prefix_len > 0){
        printf("returned model: %s\n", model_groups[max_prefix_len_group].models[model_index]);
        return model_groups[max_prefix_len_group].name;
    }

    return NULL;
}

//Эта функция возвращает поле 'name' структуры 'Group', которая содержит указанную модель диска '*model'
//В данном варианте реализован поиск модели по точному совпадению
/*
char *FindGroupByModel(const char *model) {
    for (int i = 0; i < model_group_count; i++) {
        for (int j = 0; j < model_groups[i].model_count; j++) {
            if (strcmp(model_groups[i].models[j], model) == 0) {
                return model_groups[i].name;
            }
        }
    }
    return NULL;
}
*/


int FindSmartByte (const char* profile_name, const char* attribute_name){
    if (sata_dict == NULL){
        fprintf(stderr, "Warning: 'FindSmartByte()' didnt recived file descriptor. This function stop its work.\n");
        return -1;
    }
    fseek(sata_dict, 0, 0);

    int byte = -1;
    char line[MAX_STRING_LEN];
    bool in_profile_section = false;
    int profile_name_len = strlen(profile_name)+1;
    int attribute_name_len = strlen(attribute_name)+1;
    while (fgets(line, sizeof(line), sata_dict)) {
        TrimString(line);
        if (line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            if (!in_profile_section){
                in_profile_section = (strncmp(&line[1], profile_name, profile_name_len-1) == 0 && line[profile_name_len] == ']');
                continue;
            } else {
                break;
            }
        }
        if (!in_profile_section) continue;

        char *equivalent = strchr(line, '=');
        if (!equivalent) continue;

        *equivalent = '\0';
        char *file_attribute_name = line;
        char *file_attribute_byte = equivalent + 1;

        TrimString(file_attribute_name);
        TrimString(file_attribute_byte);

        if (strncmp(file_attribute_name, attribute_name, attribute_name_len) == 0){
            sscanf(file_attribute_byte, "%d", &byte);
            fseek(sata_dict, 0, 0);
            return byte;
        }
    }
    fseek(sata_dict, 0, 0);
    return byte;
}


int FindDefaultSmartByte (const char* attribute_name){
    if (sata_dict == NULL){
        fprintf(stderr, "Warning: 'FindDefaultSmartByte()' didnt recived file descriptor. This function stop its work.\n");
        return -1;
    }
    fseek(sata_dict, 0, 0);

    int byte = -1;
    char line[MAX_STRING_LEN];
    bool in_profile_section = false;
    int attribute_name_len = strlen(attribute_name)+1;
    while (fgets(line, sizeof(line), sata_dict)) {
        TrimString(line);
        if (line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            if (!in_profile_section){
                in_profile_section = (strncmp(line, "[Profile_Default]", 17) == 0);
                continue;
            } else {
                break;
            }
        }
        if (!in_profile_section) continue;

        char *equivalent = strchr(line, '=');
        if (!equivalent) continue;

        *equivalent = '\0';
        char *file_attribute_name = line;
        char *file_attribute_byte = equivalent + 1;

        TrimString(file_attribute_name);
        TrimString(file_attribute_byte);

        if (strncmp(file_attribute_name, attribute_name, attribute_name_len) == 0){
            sscanf(file_attribute_byte, "%d", &byte);
            fseek(sata_dict, 0, 0);
            return byte;
        }
    }
    fseek(sata_dict, 0, 0);
    return byte;
}



//Эта функция посылает команды SATA устройству для получения информации о нём и его SMART параметрах и выводит эту информацию.
//В случае ошибки возвращает '-1'
int SataScan(HANDLE handle, const char *model){
    if (handle == INVALID_HANDLE_VALUE) {
        perror("Warning: function 'SataScan' got invalid handle value.\n");
        return -1;
    }

    STORAGE_PROPERTY_QUERY q_property = {0};
    q_property.PropertyId = StorageDeviceSeekPenaltyProperty;//флаг, указывающий получить дескриптор устройства
    q_property.QueryType  = PropertyStandardQuery;//флаг, указывающий получить дескриптор (или в ином случае: получить подтверждение о поддержке конкретного дескриптора)
    DEVICE_SEEK_PENALTY_DESCRIPTOR desc = { 0 };
    DWORD returned_bytes = 0;
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &q_property, sizeof(q_property), &desc, sizeof(desc), &returned_bytes, NULL)){

        bool rotating = desc.IncursSeekPenalty;
        printf("  Rotating: ");
        if (rotating) printf("yes\n");
        else printf("no\n");
    } else {
        fprintf(stderr, "[DeviceIoControl] IOCTL_STORAGE_QUERY_PROPERTY failed. Errcode: %lu\n", GetLastError());
    }

    DWORD returned = 0;
    SCSI_PASS_THROUGH_WITH_BUFFERS sptwb = {0};
    SCSI_PASS_THROUGH *spt = &sptwb.spt;
    UCHAR *cdb = spt->Cdb;
    spt->Length = sizeof(SCSI_PASS_THROUGH);
    spt->CdbLength = 16;
    spt->DataIn = SCSI_IOCTL_DATA_IN;
    spt->DataTransferLength = sizeof(sptwb.DataBuf);
    spt->TimeOutValue = 10;
    spt->DataBufferOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf);
    spt->SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, SenseBuf);
    spt->SenseInfoLength = sizeof(sptwb.SenseBuf);
    cdb[0] = SG_ATA_16;
    cdb[1] = (0x4 << 1);
    cdb[2] = (1 << 3) | (0x2 << 2) | 1;
    cdb[4] = SMART_READ_DATA;
    cdb[6] = 0x01;
    cdb[10] = SMART_CYL_LOW;
    cdb[12] = SMART_CYL_HIGH;
    cdb[14] = ATA_SMART_CMD;

    if (!DeviceIoControl(handle, IOCTL_SCSI_PASS_THROUGH, &sptwb, sizeof(sptwb), &sptwb, sizeof(sptwb), &returned, NULL)) {
        fprintf(stderr, "[DeviceIoControl] IOCTL_SCSI_PASS_THROUGH failed. Errcode: %lu\n", GetLastError());
        return -1;
    }

    if (spt->ScsiStatus != 0) {
        fprintf(stderr, "SCSI command error: status=%x, response code=%x\n", spt->ScsiStatus, (sptwb.SenseBuf[0] & 0x7F));
        if ((sptwb.SenseBuf[0] & 0x7F) == 0x70 || (sptwb.SenseBuf[0] & 0x7F) == 0x71){
            printf("sense key=%x\n", (sptwb.SenseBuf[1] & 0xF));
            printf("additional sense code=%x\n", sptwb.SenseBuf[12]);
            printf("additional sense code qualifier=%x\n", sptwb.SenseBuf[13]);
            printf("For more information see official specification\n\n");
        } else if ((sptwb.SenseBuf[0] & 0x7F) == 0x72 || (sptwb.SenseBuf[0] & 0x7F) == 0x73){
            printf("sense key=%x\n", (sptwb.SenseBuf[1] & 0xF));
            printf("additional sense code=%x\n", sptwb.SenseBuf[2]);
            printf("additional sense code qualifier=%x\n", sptwb.SenseBuf[3]);
            printf("For more information see official specification\n\n");
        } else{
            printf("returned unknown response code.\n\n");
        }
        return -1;
    }

    printf("  SMART data:");
    unsigned char checksum = 0;
    bool only_zeros = true;
    bool only_ffs = true;
    for (int i = 0; i < SMART_DATA_SIZE_BYTES; i++) {
        if ((sptwb.DataBuf[i] & 0xFF) != 0) only_zeros = false;
        if ((sptwb.DataBuf[i] & 0xFF) != 0xFF) only_ffs = false;
        if (i % 16 == 0) printf("\n    %03X: ", i);
        printf("%02X ", sptwb.DataBuf[i]);
        checksum += sptwb.DataBuf[i];
    }
    printf("\n");
    printf("    Checksum=%d\n", checksum);
    if (checksum != 0 || only_zeros || only_ffs){
        fprintf(stderr, "Warning: Invalid checksum or other parameters of SMART table. Result will be incorrect.\n");
    }
    
    SmartData smartData = {0};
    loadSataSMARTAttributes(&smartData, sptwb.DataBuf);



    returned = 0;
    memset(&sptwb, 0, sizeof(sptwb));

    spt->Length = sizeof(SCSI_PASS_THROUGH);
    spt->CdbLength = 16;
    spt->DataIn = SCSI_IOCTL_DATA_IN;
    spt->DataTransferLength = sizeof(sptwb.DataBuf);
    spt->TimeOutValue = 10;
    spt->DataBufferOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf);
    spt->SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, SenseBuf);
    spt->SenseInfoLength = sizeof(sptwb.SenseBuf);

    cdb[0] = SG_ATA_16;
    cdb[1] = (0x4 << 1);
    cdb[2] = (1 << 3) | (0x2 << 2) | 1;
    cdb[4] = SMART_READ_THRESHOLDS;
    cdb[6] = 0x01;
    cdb[10] = SMART_CYL_LOW;
    cdb[12] = SMART_CYL_HIGH;
    cdb[14] = ATA_SMART_CMD;

    if (!DeviceIoControl(handle, IOCTL_SCSI_PASS_THROUGH, &sptwb, sizeof(sptwb), &sptwb, sizeof(sptwb), &returned, NULL)) {
        fprintf(stderr, "[DeviceIoControl] IOCTL_SCSI_PASS_THROUGH failed. Errcode: %lu\n", GetLastError());
        return -1;
    }

    if (spt->ScsiStatus != 0) {
        fprintf(stderr, "SCSI command error: status=%x, response code=%x\n", spt->ScsiStatus, (sptwb.SenseBuf[0] & 0x7F));
        if ((sptwb.SenseBuf[0] & 0x7F) == 0x70 || (sptwb.SenseBuf[0] & 0x7F) == 0x71){
            printf("sense key=%x\n", (sptwb.SenseBuf[1] & 0xF));
            printf("additional sense code=%x\n", sptwb.SenseBuf[12]);
            printf("additional sense code qualifier=%x\n", sptwb.SenseBuf[13]);
            printf("For more information see official specification\n\n");
        } else if ((sptwb.SenseBuf[0] & 0x7F) == 0x72 || (sptwb.SenseBuf[0] & 0x7F) == 0x73){
            printf("sense key=%x\n", (sptwb.SenseBuf[1] & 0xF));
            printf("additional sense code=%x\n", sptwb.SenseBuf[2]);
            printf("additional sense code qualifier=%x\n", sptwb.SenseBuf[3]);
            printf("For more information see official specification\n\n");
        } else{
            printf("returned unknown response code.\n\n");
        }
        return -1;
    }

    loadSataSMARTThresholds(&smartData, sptwb.DataBuf);

    for (size_t i = 0; i < SATA_SMART_ATTRIBUTES_COUNT; i++){
        SataSMARTAttribute currentAttribute = smartData.attributes[i].attribute;
        SataSMARTThreshold currentThreshold = smartData.attributes[i].threshold;
        if (currentAttribute.id == 0){
            if (currentThreshold.id != 0){
                printf("WARNING: unexisted attribute has threshold id=%hu\n", currentThreshold.id);
            }
            continue;
        }
        printf("  attribute id(%hu):\timportant=%s\tvalue=%hu,\tworst=%hu\t", currentAttribute.id, (currentAttribute.flags[0] & 1) ? "true" : "false", currentAttribute.normalized, currentAttribute.worst);
        if (currentThreshold.id == 0) printf("threshold=undefined\n");
        else printf("threshold=%hu\n", currentThreshold.threshold);

    }



    BYTE buffer[sizeof(ATA_PASS_THROUGH_EX) + 512];
    memset(buffer, 0, sizeof(buffer));

    ATA_PASS_THROUGH_EX *pt = (ATA_PASS_THROUGH_EX*)buffer;
    pt->Length = sizeof(ATA_PASS_THROUGH_EX);
    pt->TimeOutValue = 5;
    pt->AtaFlags = ATA_FLAGS_DRDY_REQUIRED;

    pt->CurrentTaskFile[0] = SMART_RETURN_STATUS_FEATURE;
    pt->CurrentTaskFile[3] = 0x4F; // LBA Mid
    pt->CurrentTaskFile[4] = 0xC2; // LBA High
    pt->CurrentTaskFile[6] = SMART_RETURN_STATUS;

    returned_bytes = 0;

    if (!DeviceIoControl(handle,
                         IOCTL_ATA_PASS_THROUGH,
                         buffer, sizeof(buffer),
                         buffer, sizeof(buffer),
                         &returned_bytes, NULL))
    {
        fprintf(stderr, "[DeviceIoControl] IOCTL_ATA_PASS_THROUGH failed. Errcode: %lu\n", GetLastError());
        return -1;
    }

    BYTE cyl_lo = pt->CurrentTaskFile[3];
    BYTE cyl_hi = pt->CurrentTaskFile[4];
    bool error_bit = pt->CurrentTaskFile[6] & 1;

    if (error_bit)  fprintf(stderr, "[DeviceIoControl] IOCTL_ATA_PASS_THROUGH warning. Error bit = 1\n");
    else if (cyl_lo == 0x4F && cyl_hi == 0xC2) printf("    SMART status: ok\n"); // OK
    else if (cyl_lo == 0xF4 && cyl_hi == 0x2C) printf("    SMART status: not ok\n"); // FAIL
    else  fprintf(stderr, "[DeviceIoControl] IOCTL_ATA_PASS_THROUGH warning. Undefined status: cyl_low = 0x%X, cyl_hi = 0x%X\n", cyl_lo, cyl_hi);



    /*
    char* profile_name = FindGroupByModel(model);
    int Seek_Error = -1;
    int Reallocated_Sectors_Count = -1;
    if (profile_name != NULL){
        Seek_Error = FindSmartByte(profile_name, "Seek_Error");
        if (Seek_Error < 0){
            fprintf(stderr, "Warning: 'FindSmartByte()' function cant return correct value for 'Seek_Error'. Using default profile. Result may be incorrect.\n");
            Seek_Error = FindDefaultSmartByte("Seek_Error");
        }
        if (Seek_Error < 0) fprintf(stderr, "Warning: 'FindDefaultSmartByte()' function cant return correct value for 'Seek_Error'. Result will be incorrect.\n");

        Reallocated_Sectors_Count = FindSmartByte(profile_name, "Reallocated_Sectors_Count");
        if (Reallocated_Sectors_Count < 0){
            fprintf(stderr, "Warning: 'FindSmartByte()' function cant return correct value for 'Reallocated_Sectors_Count'. Using default profile. Result may be incorrect.\n");
            Reallocated_Sectors_Count = FindDefaultSmartByte("Reallocated_Sectors_Count");
        }
        if (Reallocated_Sectors_Count  < 0) fprintf(stderr, "Warning: 'FindDefaultSmartByte()' function cant return correct value for 'Reallocated_Sectors_Count'. Result will be incorrect.\n");
    } else {
        fprintf(stderr, "Warning: didnt find profile for '%s' in 'sata_dict.ini'. Result may be incorrect.\n", model);

        Seek_Error = FindDefaultSmartByte("Seek_Error");
        if (Seek_Error < 0) fprintf(stderr, "Warning: 'FindDefaultSmartByte()' function cant return correct value for 'Seek_Error'. Result will be incorrect.\n");

        Reallocated_Sectors_Count = FindDefaultSmartByte("Reallocated_Sectors_Count");
        if (Reallocated_Sectors_Count  < 0) fprintf(stderr, "Warning: 'FindDefaultSmartByte()' function cant return correct value for 'Reallocated_Sectors_Count'. Result will be incorrect.\n");
    }

    printf("  Seek_Error=%d\n", Seek_Error);
    printf("  Reallocated_Sectors_Count=%d\n", Reallocated_Sectors_Count);
    */
    printf("\n");
    
}

//Эта функция посылает команды NVMe устройству для получения информации о нём и его SMART параметрах и выводит эту информацию.
//В случае ошибки возвращает '-1'
int NvmeScan(HANDLE handle, const char *model){
    if (handle == INVALID_HANDLE_VALUE) {
        perror("Warning: function 'NvmeScan' got invalid handle value.\n");
        return -1;
    }

    DWORD returnedLength;
    size_t bufferLength = max(sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + sizeof(NVME_HEALTH_INFO_LOG), sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) + sizeof(NVME_HEALTH_INFO_LOG));
    void* buffer = calloc(1, bufferLength);
    PSTORAGE_PROPERTY_QUERY pquery = buffer;
    PSTORAGE_PROTOCOL_DATA_DESCRIPTOR protocolDataDescr = buffer;
    PSTORAGE_PROTOCOL_SPECIFIC_DATA protocolData = (PSTORAGE_PROTOCOL_SPECIFIC_DATA)pquery->AdditionalParameters;

    pquery->PropertyId = StorageDeviceProtocolSpecificProperty;  
    pquery->QueryType = PropertyStandardQuery;

    protocolData->ProtocolType = ProtocolTypeNvme;  
    protocolData->DataType = NVMeDataTypeLogPage;  
    protocolData->ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO;  
    protocolData->ProtocolDataRequestSubValue = 0;  // This will be passed as the lower 32 bit of log page offset if controller supports extended data for the Get Log Page.
    protocolData->ProtocolDataRequestSubValue2 = 0; // This will be passed as the higher 32 bit of log page offset if controller supports extended data for the Get Log Page.
    protocolData->ProtocolDataRequestSubValue3 = 0; // This will be passed as Log Specific Identifier in CDW11.
    protocolData->ProtocolDataRequestSubValue4 = 0; // This will map to STORAGE_PROTOCOL_DATA_SUBVALUE_GET_LOG_PAGE definition, then user can pass Retain Asynchronous Event, Log Specific Field.

    protocolData->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);  
    protocolData->ProtocolDataLength = sizeof(NVME_HEALTH_INFO_LOG);
    
    int result = DeviceIoControl(handle,  
                                IOCTL_STORAGE_QUERY_PROPERTY,  
                                buffer,  
                                bufferLength,  
                                buffer, 
                                bufferLength,  
                                &returnedLength,  
                                NULL);

    if (!result || (returnedLength == 0)) {
        fprintf(stderr, "[DeviceIoControl] IOCTL_STORAGE_QUERY_PROPERTY failed. Errcode: %lu\n \
            [DeviceIoControl] result: %d\n \
            [DeviceIoControl] returned length: %lu\n", 
            GetLastError(), result, returnedLength);
        free(buffer);
        return -1;
    }

    if ((protocolDataDescr->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) || (protocolDataDescr->Size != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR))) {
        //printf("DeviceNVMeQueryProtocolDataTest: SMART/Health Information Log - data descriptor header not valid.\n");
        fprintf(stderr, "[DeviceIoControl] IOCTL_STORAGE_QUERY_PROPERTY failed: data descriptor header not valid.\n \
            [DeviceIoControl] data descriptor version: %lu (should be %lu)\n \
            [DeviceIoControl] data descriptor size: %lu (should be %lu)\n",
            protocolDataDescr->Version, sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR), protocolDataDescr->Size, sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR));
        free(buffer);
        return -1;
    }

    protocolData = &protocolDataDescr->ProtocolSpecificData;

    if ((protocolData->ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)) || (protocolData->ProtocolDataLength < sizeof(NVME_HEALTH_INFO_LOG))) {
        //printf("DeviceNVMeQueryProtocolDataTest: SMART/Health Information Log - ProtocolData Offset/Length not valid.\n");
        fprintf(stderr, "[DeviceIoControl] IOCTL_STORAGE_QUERY_PROPERTY failed: ProtocolData Offset/Length not valid.\n \
            [DeviceIoControl] protocolData offset: %lu (should be at least %lu)\n \
            [DeviceIoControl] protocolData length: %lu (should be at least %lu)\n",
            protocolData->ProtocolDataOffset, sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA), protocolData->ProtocolDataLength, sizeof(NVME_HEALTH_INFO_LOG));
        free(buffer);
        return -1;
    }

    PNVME_HEALTH_INFO_LOG smartInfo = (PNVME_HEALTH_INFO_LOG)((PCHAR)protocolData + protocolData->ProtocolDataOffset);

    printf("  SMART data:");
    for (int i = 0; i < SMART_DATA_SIZE_BYTES; i++) {
        if (i % 16 == 0) printf("\n    %03X: ", i);
        printf("%02X ", ((UCHAR*)smartInfo)[i]);
    }
    printf("\n");

    UINT16 temperature = 0;
    UCHAR egcws = smartInfo->Reserved0[0];
    ULONG64 dur_low = 0;
    ULONG64 dur_high = 0;
    ULONG64 duw_low = 0;
    ULONG64 duw_high = 0;
    ULONG64 hrc_low = 0;
    ULONG64 hrc_high = 0;
    ULONG64 hwc_low = 0;
    ULONG64 hwc_high = 0;
    ULONG64 cbt_low = 0;
    ULONG64 cbt_high = 0;
    ULONG64 pwrc_low = 0;
    ULONG64 pwrc_high = 0;
    ULONG64 poh_low = 0;
    ULONG64 poh_high = 0;
    ULONG64 upl_low = 0;
    ULONG64 upl_high = 0;
    ULONG64 mdie_low = 0;
    ULONG64 mdie_high = 0;
    ULONG64 neile_low = 0;
    ULONG64 neile_high = 0;
    UINT32 tmt1tc = 0;
    UINT32 tmt2tc = 0;
    UINT32 tttmt1 = 0;
    UINT32 tttmt2 = 0;
    ULONG64 olec = 0;
    UINT32 ipm = 0;
    memcpy(&temperature, smartInfo->Temperature, sizeof(UINT16));
    memcpy(&dur_low, smartInfo->DataUnitRead, sizeof(ULONG64));
    memcpy(&dur_high, &smartInfo->DataUnitRead[8], sizeof(ULONG64));
    memcpy(&duw_low, smartInfo->DataUnitWritten, sizeof(ULONG64));
    memcpy(&duw_high, &smartInfo->DataUnitWritten[8], sizeof(ULONG64));
    memcpy(&hrc_low, smartInfo->HostReadCommands, sizeof(ULONG64));
    memcpy(&hrc_high, &smartInfo->HostReadCommands[8], sizeof(ULONG64));
    memcpy(&hwc_low, smartInfo->HostWrittenCommands, sizeof(ULONG64));
    memcpy(&hwc_high, &smartInfo->HostWrittenCommands[8], sizeof(ULONG64));
    memcpy(&cbt_low, smartInfo->ControllerBusyTime, sizeof(ULONG64));
    memcpy(&cbt_high, &smartInfo->ControllerBusyTime[8], sizeof(ULONG64));
    memcpy(&pwrc_low, smartInfo->PowerCycle, sizeof(ULONG64));
    memcpy(&pwrc_high, &smartInfo->PowerCycle[8], sizeof(ULONG64));
    memcpy(&poh_low, smartInfo->PowerOnHours, sizeof(ULONG64));
    memcpy(&poh_high, &smartInfo->PowerOnHours[8], sizeof(ULONG64));
    memcpy(&upl_low, smartInfo->UnsafeShutdowns, sizeof(ULONG64));
    memcpy(&upl_high, &smartInfo->UnsafeShutdowns[8], sizeof(ULONG64));
    memcpy(&mdie_low, smartInfo->MediaErrors, sizeof(ULONG64));
    memcpy(&mdie_high, &smartInfo->MediaErrors[8], sizeof(ULONG64));
    memcpy(&neile_low, smartInfo->ErrorInfoLogEntryCount, sizeof(ULONG64));
    memcpy(&neile_high, &smartInfo->ErrorInfoLogEntryCount[8], sizeof(ULONG64));
    memcpy(&tmt1tc, smartInfo->Reserved1, sizeof(UINT32));
    memcpy(&tmt2tc, &smartInfo->Reserved1[4], sizeof(UINT32));
    memcpy(&tttmt1, &smartInfo->Reserved1[8], sizeof(UINT32));
    memcpy(&tttmt2, &smartInfo->Reserved1[12], sizeof(UINT32));
    memcpy(&olec, &smartInfo->Reserved1[16], sizeof(ULONG64));
    memcpy(&ipm, &smartInfo->Reserved1[24], sizeof(UINT32));

    printf("  Composite Temperature=                            %hu\n", temperature);
    printf("  Available Spare=                                  %hu\n", smartInfo->AvailableSpare);
    printf("  Available Spare Threshold=                        %hu\n", smartInfo->AvailableSpareThreshold);
    printf("  Percentage Used=                                  %hu\n", smartInfo->PercentageUsed);
    printf("  Endurance Group Critical Warning Summary=         %hu\n", smartInfo->Reserved0[0]);
    printf("  Data Units Read(Low)=                             %llu\n", dur_low);
    printf("  Data Units Read(Hight)=                           %llu\n", dur_high);
    printf("  Data Units Written(Low)=                          %llu\n", duw_low);
    printf("  Data Units Written(Hight)=                        %llu\n", duw_high);
    printf("  Host Read Commands(Low)=                          %llu\n", hrc_low);
    printf("  Host Read Commands(Hight)=                        %llu\n", hrc_high);
    printf("  Host Write Commands(Low)=                         %llu\n", hwc_low);
    printf("  Host Write Commands(Hight)=                       %llu\n", hwc_high);
    printf("  Controller Busy Time(Low)=                        %llu\n", cbt_low);
    printf("  Controller Busy Time(Hight)=                      %llu\n", cbt_high);
    printf("  Power Cycles(Low)=                                %llu\n", pwrc_low);
    printf("  Power Cycles(Hight)=                              %llu\n", pwrc_high);
    printf("  Power On Hours(Low)=                              %llu\n", poh_low);
    printf("  Power On Hours(Hight)=                            %llu\n", poh_high);
    printf("  Unexpected Power Losses(Low)=                     %llu\n", upl_low);
    printf("  Unexpected Power Losses(Hight)=                   %llu\n", upl_high);
    printf("  Media and Data Integrity Errors(Low)=             %llu\n", mdie_low);
    printf("  Media and Data Integrity Errors(Hight)=           %llu\n", mdie_high);
    printf("  Number of Error Information Log Entries(Low)=     %llu\n", neile_low);
    printf("  Number of Error Information Log Entries(Hight)=   %llu\n", neile_high);
    printf("  Warning Composite Temperature Time=               %u\n", smartInfo->WarningCompositeTemperatureTime);
    printf("  Critical Composite Temperature Time=              %u\n", smartInfo->CriticalCompositeTemperatureTime);
    printf("  Temperature Sensor 1=                             %hu\n", smartInfo->TemperatureSensor1);
    printf("  Temperature Sensor 2=                             %hu\n", smartInfo->TemperatureSensor2);
    printf("  Temperature Sensor 3=                             %hu\n", smartInfo->TemperatureSensor3);
    printf("  Temperature Sensor 4=                             %hu\n", smartInfo->TemperatureSensor4);
    printf("  Temperature Sensor 5=                             %hu\n", smartInfo->TemperatureSensor5);
    printf("  Temperature Sensor 6=                             %hu\n", smartInfo->TemperatureSensor6);
    printf("  Temperature Sensor 7=                             %hu\n", smartInfo->TemperatureSensor7);
    printf("  Temperature Sensor 8=                             %hu\n", smartInfo->TemperatureSensor8);
    printf("  Thermal Management Temperature 1 Transition Count=%u\n", tmt1tc);
    printf("  Thermal Management Temperature 2 Transition Count=%u\n", tmt2tc);
    printf("  Total Time For Thermal Management Temperature 1=  %u\n", tttmt1);
    printf("  Total Time For Thermal Management Temperature 2=  %u\n", tttmt2);
    printf("  Operational Lifetime Energy Consumed=             %llu\n", olec);
    printf("  Interval Power Measurement=                       %u\n", ipm);


    printf("\n  SMART status: ");
    if ((((UCHAR*)smartInfo)[0] & 0xFF) != 0x0){
        printf("not ok.\n");
        printf("    Critical Warning byte=0x%X. Problems:\n", ((UCHAR*)smartInfo)[0]);
        
        if(smartInfo->CriticalWarning.AvailableSpaceLow){
            printf("      Available Spare Capacity Below Threshold (ASCBT)\n");
        }
        if(smartInfo->CriticalWarning.TemperatureThreshold){
            printf("      Temperature Threshold Condition (TTC)\n");
        }
        if(smartInfo->CriticalWarning.ReliabilityDegraded){
            printf("      NVM Subsystem Degraded Reliability (NDR)\n");
        }
        if(smartInfo->CriticalWarning.ReadOnly){
            printf("      All Media Read-Only (AMRO)\n");
        }
        if(smartInfo->CriticalWarning.VolatileMemoryBackupDeviceFailed){
            printf("      Volatile Memory Backup Failed (VMBF)\n");
        }
        if((smartInfo->CriticalWarning.AsUchar & (0x1 << 5))){
            printf("      Persistent Memory Region Read-Only (PMRRO)\n");
        }
        if((smartInfo->CriticalWarning.AsUchar & (0x1 << 6))){
            printf("      Indeterminate Personality State (IPS)\n");
        }
    } else {
        printf("ok.\n");
    }

    printf("\n");
    free(buffer);
    return 0;
    
}


int main() {
    HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_DISK, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (hDevInfo == INVALID_HANDLE_VALUE) {//например здесь может вернуть не только INVALID_HANDLE_VALUE. Более качественно проверять на ошибки
        fprintf(stderr, "SetupDiGetClassDevs failed. Errcode: %lu\n", GetLastError());
        return 1;
    }

    sata_dict = fopen("sata_dict.ini", "r");
    if (!sata_dict){
        perror("[fopen] Error");
        return 0;
    }

    LoadGroups(sata_dict);

    bool any_disk_exists = false;
    SP_DEVICE_INTERFACE_DATA ifData = {0};
    ifData.cbSize = sizeof(ifData);
    printf("Scanning disks...\n\n");
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_DISK, i, &ifData); ++i){
        printf("\nDisk %lu:\n", i);
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, NULL, 0, &required, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA pDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(required);
        pDetail->cbSize = sizeof(PSP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, pDetail, required, NULL, NULL)) {
            fprintf(stderr, "[SetupDiGetDeviceInterfaceDetail] failed. Errcode: %lu\n", GetLastError());
            free(pDetail);
            continue;
        }

        HANDLE handle = CreateFile(pDetail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "[CreateFile] failed. Errcode: %lu\nDevicePath: %S\n", GetLastError(), pDetail->DevicePath);
            free(pDetail);
            if (GetLastError() == 5){
                fprintf(stderr, "If Errcode=5, programm hasn't got permission to do this operation. Probably, you should start this programm in admin mode.\n");
            }
            continue;
        }

        STORAGE_PROPERTY_QUERY q_property = {0};
        q_property.PropertyId = StorageDeviceProperty;//флаг, указывающий получить дескриптор устройства
        q_property.QueryType  = PropertyStandardQuery;//флаг, указывающий получить дескриптор (или в ином случае: получить подтверждение о поддержке конкретного дескриптора)

        BYTE buf[4096];
        DWORD returned_bytes = 0;

        if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &q_property, sizeof(q_property), buf, sizeof(buf), &returned_bytes, NULL)){
            STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR*)buf;

            char *model  = desc->ProductIdOffset       ? (char*)buf + desc->ProductIdOffset       : NULL;
            char *vendor = desc->VendorIdOffset        ? (char*)buf + desc->VendorIdOffset        : NULL;
            char *rev    = desc->ProductRevisionOffset ? (char*)buf + desc->ProductRevisionOffset : NULL;
            char *serial = desc->SerialNumberOffset    ? (char*)buf + desc->SerialNumberOffset    : NULL;
            STORAGE_BUS_TYPE BusType = desc->BusType   ? desc->BusType                            : 0x00;

            TrimString(model);
            TrimString(serial);
            
            printf("  Model   : %s\n",  model);
            printf("  Serial  : %s\n",  serial);
            printf("  Bus type: ");
            if (BusType == BusTypeUnknown) printf("unknown\n");
            else if (BusType == BusTypeScsi)  printf("SCSI\n");
            else if (BusType == BusTypeAtapi)  printf("ATAPI\n");
            else if (BusType == BusTypeAta)  printf("ATA\n");
            else if (BusType == BusType1394)  printf("1394\n");
            else if (BusType == BusTypeSsa)  printf("SSA\n");
            else if (BusType == BusTypeFibre)  printf("FIBRE\n");
            else if (BusType == BusTypeUsb)  printf("USB\n");
            else if (BusType == BusTypeRAID)  printf("RAID\n");
            else if (BusType == BusTypeiScsi)  printf("SCSI\n");
            else if (BusType == BusTypeSas)  printf("SAS\n");
            else if (BusType == BusTypeSata)  {printf("SATA\n"); SataScan(handle, model);}
            else if (BusType == BusTypeSd)  printf("SD\n");
            else if (BusType == BusTypeMmc)  printf("MMC\n");
            else if (BusType == BusTypeVirtual)  printf("Virtual\n");
            else if (BusType == BusTypeFileBackedVirtual)  printf("FileBackendVirtual\n");
            else if (BusType == BusTypeSpaces)  printf("Spaces\n");
            else if (BusType == BusTypeNvme)  {printf("NVMe\n"); NvmeScan(handle, model);}
            else if (BusType == BusTypeSCM)  printf("SCM\n");
            else if (BusType == BusTypeUfs)  printf("UFS\n");
            else if (BusType == BusTypeMax)  printf("MAX\n");
            else printf("unsupported value\n");
        }
        else {
            fprintf(stderr, "[DeviceIoControl] IOCTL_STORAGE_QUERY_PROPERTY failed. Errcode: %lu\n", GetLastError());
        }

        CloseHandle(handle);
        free(pDetail);

    }

    if (GetLastError() != ERROR_NO_MORE_ITEMS){
        fprintf(stderr, "[SetupDiEnumDeviceInterfaces] failed. Errcode: %lu\n", GetLastError());
    }
    
    SetupDiDestroyDeviceInfoList(hDevInfo);
    printf("Press Enter to exit...");
    getchar();
    return 0;
}