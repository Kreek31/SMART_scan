#define INITGUID
#include <stdio.h>
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>
#include <ntddstor.h>
#include <ntddscsi.h>
#include <stdbool.h>

//minimum Windows version: Windows 10 (IOCTL_STORAGE_PROTOCOL_COMMAND requironment)

//TODO: IOCTL_STORAGE_QUERY_PROPERTY - посмотреть все варианты возвращаемых байтов при успешном и неуспешном запросе и переписать структуры, которые принимают эти байты.
//TODO: Реализовать неточный поиск модели в файле. Для этого, как вариант, удалять символы из полученной строки до тех пор, пока не встретим первое совпадение.
//TODO: Реализовать получение значения SMART таблицы по байтам из справочника

#define SG_ATA_16 0x85
#define ATA_SMART_CMD 0xB0
#define NVME_ADMIN_GET_LOG_PAGE 0x02
#define SMART_READ_DATA 0xD0
#define SMART_CYL_LOW 0x4F
#define SMART_CYL_HIGH 0xC2
#define SMART_DATA_SIZE_BYTES 512

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
typedef struct STORAGE_PROTOCOL_COMMAND_WITH_BUFFERS{
    STORAGE_PROTOCOL_COMMAND spc;
    UCHAR ErrorInfo[4096];
    UCHAR DataToDeviceBuffer[4096];
    UCHAR DataFromDeviceBuffer[4096];
} STORAGE_PROTOCOL_COMMAND_WITH_BUFFERS;


//Эта функция избавляется от лишних пробелов в начале и конце строки '*str'.
//Состояние '*str' при этом меняется в соответствии с работой функции.
//Пример:
//char str[7] = "  abc   ";
//TrimString(str, strlen(str)+1) <- эквивалентно записи 'str = "abc"'.
void TrimString(char *str, size_t size) {
    for (size_t i = size - 1; i >= 0 && isspace((unsigned char)str[i]); --i) {
        str[i] = '\0';
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
        TrimString(line, strlen(line)+1);
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

        TrimString(group_name, strlen(group_name)+1);
        TrimString(models_str, strlen(models_str)+1);

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
            TrimString(token, token_length);
            g->models[g->model_count] = malloc(token_length);
            strncpy(g->models[g->model_count++], token, token_length);
            token = strtok(NULL, ",\"");
        }
    }
    fseek(sata_dict, 0, 0);
}


//Эта функция возвращает поле 'name' структуры 'Group', которая содержит указанную модель диска '*model'
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
        TrimString(line, strlen(line)+1);
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

        TrimString(file_attribute_name, strlen(file_attribute_name)+1);
        TrimString(file_attribute_byte, strlen(file_attribute_byte)+1);

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
        TrimString(line, strlen(line)+1);
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

        TrimString(file_attribute_name, strlen(file_attribute_name)+1);
        TrimString(file_attribute_byte, strlen(file_attribute_byte)+1);

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
    printf("\n");
    
}

//Эта функция посылает команды NVMe устройству для получения информации о нём и его SMART параметрах и выводит эту информацию.
//В случае ошибки возвращает '-1'
int NvmeScan(HANDLE handle, const char *model){
    if (handle == INVALID_HANDLE_VALUE) {
        perror("Warning: function 'SataScan' got invalid handle value.\n");
        return -1;
    }

    STORAGE_PROTOCOL_COMMAND_WITH_BUFFERS spcwb = {0};
    STORAGE_PROTOCOL_COMMAND *spc = &spcwb.spc;

    spc->Version = STORAGE_PROTOCOL_STRUCTURE_VERSION;
    spc->Length = sizeof(STORAGE_PROTOCOL_COMMAND);
    spc->ProtocolType = ProtocolTypeNvme;
    spc->Flags = STORAGE_PROTOCOL_COMMAND_FLAG_ADAPTER_REQUEST;
    spc->CommandLength = 64;
    spc->ErrorInfoLength = sizeof(spcwb.ErrorInfo);
    spc->DataToDeviceTransferLength = sizeof(spcwb.DataToDeviceBuffer);
    spc->DataFromDeviceTransferLength = sizeof(spcwb.DataFromDeviceBuffer);
    spc->TimeOutValue = 10;
    spc->ErrorInfoOffset = offsetof(STORAGE_PROTOCOL_COMMAND_WITH_BUFFERS, ErrorInfo);
    spc->DataToDeviceBufferOffset = offsetof(STORAGE_PROTOCOL_COMMAND_WITH_BUFFERS, DataToDeviceBuffer);
    spc->DataFromDeviceBufferOffset = offsetof(STORAGE_PROTOCOL_COMMAND_WITH_BUFFERS, DataFromDeviceBuffer);

    //структура command_field задается в NVM Express® Base Specification в таблице 92: Common Command Format
    UCHAR *command_field = spc->Command;
    memset(command_field, 0, 64);//<--------------------------------------выход за пределы памяти?

    /*
    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE;//<-----------------------поле opcode имеет
    cmd.nsid = 0xFFFFFFFF;
    cmd.addr = (uint64_t)&smart_log;
    cmd.data_len = sizeof(smart_log);
    cmd.cdw10 = ((SMART_DATA_SIZE_DWORDS & 0xFFFF) << 16) | (SMART_LID & 0xFF);

    if (!DeviceIoControl(handle, IOCTL_STORAGE_PROTOCOL_COMMAND, &protocolCmd, ...)) {
        fprintf(stderr, "[DeviceIoControl] IOCTL_STORAGE_PROTOCOL_COMMAND failed. Errcode: %lu\n", GetLastError());
        return -1;
    }

    printf("  SMART data:");
    for (int i = 0; i < SMART_DATA_SIZE_BYTES; i++) {
        if (i % 16 == 0) printf("\n    %03X: ", i);
        printf("%02X ", smart_log[i]);
    }

    printf("\n  SMART status: ");
    if ((smart_log[0] & 0xFF) != 0x0){
        printf("not ok.\n");
        printf("    Critical Warning byte=0x%X. Problems:\n", smart_log[0]);
        
        if((smart_log[0] & 0x1) == 0x1){
            printf("      Available Spare Capacity Below Threshold (ASCBT)\n");
        }
        if((smart_log[0] & (0x1 << 1)) == 0x1){
            printf("      Temperature Threshold Condition (TTC)\n");
        }
        if((smart_log[0] & (0x1 << 2)) == 0x1){
            printf("      NVM Subsystem Degraded Reliability (NDR)\n");
        }
        if((smart_log[0] & (0x1 << 3)) == 0x1){
            printf("      All Media Read-Only (AMRO)\n");
        }
        if((smart_log[0] & (0x1 << 4)) == 0x1){
            printf("      Volatile Memory Backup Failed (VMBF)\n");
        }
        if((smart_log[0] & (0x1 << 5)) == 0x1){
            printf("      Persistent Memory Region Read-Only (PMRRO)\n");
        }
        if((smart_log[0] & (0x1 << 6)) == 0x1){
            printf("      Indeterminate Personality State (IPS)\n");
        }
    } else {
        printf("ok.\n");
    }

    printf("\n");
    close(fd);
    */
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
        printf("Disk %lu:\n", i);
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
            fprintf(stderr, "[CreateFile] failed. DevicePath: %S\n Errcode: %lu\n",pDetail->DevicePath, GetLastError());
            free(pDetail);
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
            else if (BusType == BusTypeNvme)  printf("NVMe\n");
            else if (BusType == BusTypeSCM)  printf("SCM\n");
            else if (BusType == BusTypeUfs)  printf("UFS\n");
            else if (BusType == BusTypeMax)  printf("MAX\n");
            else printf("unsupported value\n");
        }
        else {
            fprintf(stderr, "IOCTL_STORAGE_QUERY_PROPERTY failed. Errcode: %lu\n", GetLastError());
        }

        CloseHandle(handle);
        free(pDetail);

    }

    if (GetLastError() != ERROR_NO_MORE_ITEMS){
        fprintf(stderr, "SetupDiEnumDeviceInterfaces failed. Errcode: %lu\n", GetLastError());
    }
    
    SetupDiDestroyDeviceInfoList(hDevInfo);
    printf("Press Enter to exit...");
    getchar();
    return 0;
}