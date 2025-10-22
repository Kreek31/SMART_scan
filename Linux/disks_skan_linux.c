#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/hdreg.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <scsi/sg.h>

//TODO:LoadGroups, FindSmartByte, FindDefaultSmartByte - Перепроверить/подкорректировать работу функции, дописать комментарии. Избавиться от магических чисел. Учесть, что функция не очень хорошо учитывает пробелы при чтении значений ключа в файле. По возможности сократить кол-во операций 'strlen' и 'TrimString'
//TODO: SdScan - добавить получение значения SMART по прочитанным байтам.

#define DEV_PATH "/dev"
#define MAX_PATH 256
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_ADMIN_GET_LOG_PAGE 0x02
#define NVME_SN_SIZE 20
#define NVME_SN_START_BIT 4
#define NVME_MN_SIZE 40
#define NVME_MN_START_BIT 24
#define SG_ATA_16 0x85
#define ATA_SMART_CMD 0xB0
#define SMART_READ_DATA 0xD0
#define SMART_READ_LOG 0xD5
#define SMART_CYL_LOW 0x4F
#define SMART_CYL_HIGH 0xC2
#define SMART_DATA_SIZE_BYTES 512
#define SMART_DATA_SIZE_DWORDS 128
#define SMART_LID 0x02

#define MAX_GROUPS 64
#define MAX_MODELS 16
#define MAX_STRING_LEN 1024

FILE * sata_dict;
int model_group_count = 0;

/*
Функции:
    1) stat() - возвращает информацию об указанном файле в указанную структуру stat
    2) ioctl() - посылает устройству указанную команду. В зависимости от команды может возвращать различные структуры, адрес которых указывается в 3-м параметре.
Структуры:
    1) stat - содержит поля, описывающие свойства некоторого файла. Данную структуру можно получить в результате работы функции stat()
    2) hd_driveid - содержит информацию идентификации жесткого диска и его перечень команд. Возвращается в результате работы ioctl() с флагом HDIO_GET_IDENTITY
    3) nvme_admin_cmd - структура, содержащая admin-команду NVMe диска и параметры данной команды
Макросы и флаги:
    1) S_ISBLK - проверяет, является ли указанный файл блочным устройством. Принимает на вход параметр stat.st_mode
    2) HDIO_GET_IDENTITY - команда функции ioctl(), которая идентифицирует жесткий диск и возвращает данные о нем в виде структуры hd_driveid
    3) NVME_ADMIN_IDENTIFY - admin-команда NVMe диска, которая в зависимости от значения поля cdw10 (command dword) структуры nvme_admin_cmd возвращает базовую информацию о диске
    4) NVME_IOCTL_ADMIN_CMD - команда функции ioctl(), которая позволяет отправлять admin-команды диску NVMe. В ioctl() требуется указать структуру nvme_admin_cmd
*/

//Структура используется для объединения множества моделей дисков в одну группу
typedef struct {
    char *name;        // Название группы (например, "Seagate")
    char *models[MAX_MODELS];     // Список моделей в группе
    int model_count;
} Group;
Group model_groups[MAX_GROUPS];


//Эта функция избавляется от лишних пробелов в начале и конце строки '*str'.
//Состояние '*str' при этом меняется в соответствии с работой функции.
//Пример:
//char str[7] = "  abc   ";
//TrimString(str, strlen(str)+1) <- эквивалентно записи 'str = "abc"'.
void TrimString(char *str, size_t size) {
    for (ssize_t i = size - 1; i >= 0 && isspace((unsigned char)str[i]); --i) {
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

//Эта функция определяет, соответствует ли 'name' названию файла для SATA устройства.
//Это происходит посредством проверки каждого символа 'name' на допустимое значение.
int IsSdDevice(const char *name) {
    size_t len = strlen(name);
    return len == 3 && strncmp(name, "sd", 2) == 0 && isalpha(name[2]);
}

//Эта функция определяет, соответствует ли 'name' названию файла для NVMe устройства.
//Работает аналогично функции 'IsSdDevice'.
int IsNvmeDevice(const char *name) {
    size_t len = strlen(name);
    return strncmp(name, "nvme", 4) == 0 && isdigit(name[4]) && name[5] == 'n' && isdigit(name[6]) && name[7] == '\0';
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
int SdScan(const char *path){
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0){
        perror("[open()] Error");
        close(fd);
        return -1;
    }

    struct hd_driveid id;
    int rotation_rate;
    if (ioctl(fd, HDIO_GET_IDENTITY, &id) >= 0) {
        rotation_rate = id.words206_254[11];
        TrimString(id.model, sizeof(id.model));
        TrimString(id.serial_no, sizeof(id.serial_no));

        printf("Device: %s\n", path);
        printf("  Model:         %.40s\n", id.model);
        printf("  Serial:        %.20s\n", id.serial_no);
        printf("  Rotation rate: %d\n", rotation_rate);
    } else {
        perror("[ioctl()] returned negative value. Error");
        close(fd);
        return -1;
    }

    unsigned char cdb[16] = {0};
    unsigned char sense[32] = {0};
    unsigned char data[SMART_DATA_SIZE_BYTES] = {0};

    cdb[0] = SG_ATA_16;
    cdb[1] = (0x4 << 1);
    cdb[2] = (1 << 3) | (0x2 << 2) | 1;
    cdb[4] = SMART_READ_DATA;
    cdb[6] = 0x01;
    cdb[10] = SMART_CYL_LOW;
    cdb[12] = SMART_CYL_HIGH;
    cdb[14] = ATA_SMART_CMD;

    sg_io_hdr_t io_hdr;
    memset(&io_hdr, 0, sizeof(io_hdr));

    io_hdr.interface_id = 'S';
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.cmdp = cdb;
    io_hdr.cmd_len = sizeof(cdb);
    io_hdr.dxferp = data;
    io_hdr.dxfer_len = sizeof(data);
    io_hdr.sbp = sense;
    io_hdr.mx_sb_len = sizeof(sense);
    io_hdr.timeout = 5000;

    if (ioctl(fd, SG_IO, &io_hdr) < 0) {
        perror("[ioctl SG_IO] returned negative value. Error");
        close(fd);
        return -1;
    }

    if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
        fprintf(stderr, "SG_IO error: status=%x, response code=%x\n", io_hdr.status, (sense[0] & 0x7F));
        if ((sense[0] & 0x7F) == 0x70 || (sense[0] & 0x7F) == 0x71){
            printf("sense key=%x\n", (sense[1] & 0xF));
            printf("additional sense code=%x\n", sense[12]);
            printf("additional sense code qualifier=%x\n", sense[13]);
            printf("For more information see official specification\n\n");
        } else if ((sense[0] & 0x7F) == 0x72 || (sense[0] & 0x7F) == 0x73){
            printf("sense key=%x\n", (sense[1] & 0xF));
            printf("additional sense code=%x\n", sense[2]);
            printf("additional sense code qualifier=%x\n", sense[3]);
            printf("For more information see official specification\n\n");
        } else{
            printf("returned unknown response code.\n\n");
        }
        close(fd);
        return -1;
    }

    printf("  SMART data:");
    unsigned char checksum = 0;
    bool only_zeros = true;
    bool only_ffs = true;
    for (int i = 0; i < SMART_DATA_SIZE_BYTES; i++) {
        if ((data[i] & 0xFF) != 0) only_zeros = false;
        if ((data[i] & 0xFF) != 0xFF) only_ffs = false;
        if (i % 16 == 0) printf("\n    %03X: ", i);
        printf("%02X ", data[i]);
        checksum += data[i];
    }
    printf("\n");
    printf("    Checksum=%d\n", checksum);
    if (checksum != 0 || only_zeros || only_ffs){
        fprintf(stderr, "Warning: Invalid checksum or other parameters of SMART table. Result will be incorrect.\n");
    }

    char* profile_name = FindGroupByModel(id.model);
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
        fprintf(stderr, "Warning: didnt find profile for '%s' in 'sata_dict.ini'. Result may be incorrect.\n", id.model);

        Seek_Error = FindDefaultSmartByte("Seek_Error");
        if (Seek_Error < 0) fprintf(stderr, "Warning: 'FindDefaultSmartByte()' function cant return correct value for 'Seek_Error'. Result will be incorrect.\n");

        Reallocated_Sectors_Count = FindDefaultSmartByte("Reallocated_Sectors_Count");
        if (Reallocated_Sectors_Count  < 0) fprintf(stderr, "Warning: 'FindDefaultSmartByte()' function cant return correct value for 'Reallocated_Sectors_Count'. Result will be incorrect.\n");
    }

    printf("  Seek_Error=%d\n", Seek_Error);
    printf("  Reallocated_Sectors_Count=%d\n", Reallocated_Sectors_Count);
    printf("\n");
    close(fd);
}

//Эта функция посылает команды NVMe устройству для получения информации о нём и его SMART параметрах и выводит эту информацию.
//В случае ошибки возвращает '-1'
int NvmeScan(const char *path){
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0){
        perror("[open()] Error");
        close(fd);
        return -1;
    }

    struct nvme_admin_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));

    char buf[4096] = {0};

    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.addr = (uint64_t)&buf;
    cmd.data_len = sizeof(buf);
    cmd.cdw10 = 1; 

    if (ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) >= 0) {
        char * nvme_serial = malloc(NVME_SN_SIZE + 1);
        char * nvme_model = malloc(NVME_MN_SIZE + 1);

        strncpy(nvme_serial, &buf[NVME_SN_START_BIT], NVME_SN_SIZE);
        strncpy(nvme_model, &buf[NVME_MN_START_BIT], NVME_MN_SIZE);
        TrimString(nvme_serial, NVME_SN_SIZE);
        TrimString(nvme_model, NVME_MN_SIZE);

        printf("Device: %s\n", path);
        printf("  Model:  %.40s\n", nvme_model);
        printf("  Serial: %.20s\n", nvme_serial);
    } else {
        perror("[ioctl() NVME_IOCTL_ADMIN_CMD NVME_ADMIN_IDENTIFY] returned negative value. Error");
        close(fd);
        return -1;
    }

    unsigned char smart_log[SMART_DATA_SIZE_BYTES] = {0};
    memset(&cmd, 0, sizeof(cmd));

    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE;
    cmd.nsid = 0xFFFFFFFF;
    cmd.addr = (uint64_t)&smart_log;
    cmd.data_len = sizeof(smart_log);
    cmd.cdw10 = ((SMART_DATA_SIZE_DWORDS & 0xFFFF) << 16) | (SMART_LID & 0xFF);

    if (ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) < 0) {
        perror("[ioctl NVME_IOCTL_ADMIN_CMD NVME_ADMIN_GET_LOG_PAGE] returned negative value. Error");
        close(fd);
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
}



int main() {
    DIR *dir = opendir(DEV_PATH);
    if (!dir) {
        perror("[opendir] Error");
        return 1;
    }

    sata_dict = fopen("sata_dict.ini", "r");
    if (!sata_dict){
        perror("[fopen] Error");
        return 0;
    }

    LoadGroups(sata_dict);

    struct dirent *entry;
    char path[MAX_PATH + strlen(DEV_PATH) + 1];
    struct stat st;

    printf("Scanning /dev for ATA/SATA disks...\n\n");

    bool any_disk_exists = false;
    while ((entry = readdir(dir)) != NULL) {
        snprintf(path, sizeof(path), "%s/%s", DEV_PATH, entry->d_name);

        if (stat(path, &st) != 0){
            perror("[stat()] Error");
            continue;
        }

        if(!S_ISBLK(st.st_mode)) continue;

        if (IsSdDevice(entry->d_name)){
            any_disk_exists = true;
            SdScan(path);
        }

        if (IsNvmeDevice(entry->d_name)){
            any_disk_exists = true;
            NvmeScan(path);
        }
    }

    if (!any_disk_exists){
        printf("No disks found\n");
    }

    fclose(sata_dict);
    closedir(dir);
    return 0;
}