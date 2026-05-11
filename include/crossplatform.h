#define SATA_SMART_ATTRIBUTE_SIZE 12
#define SATA_SMART_ATTRIBUTES_COUNT 256
#define SATA_SMART_BEGIN_ATTRIBUTES 2
#define SATA_SMART_END_ATTRIBUTES 361
#define SMART_READ_DATA 0xD0
#define SMART_READ_THRESHOLDS 0xD1
#define SMART_DATA_SIZE_BYTES 512

typedef struct SataSMARTAttribute {
    unsigned char id;
    unsigned char flags[2];
    unsigned char normalized;
    unsigned char worst;
    unsigned char raw[6];
    unsigned char reserved;
} SataSMARTAttribute;

typedef struct SataSMARTThreshold{
    unsigned char id;
    unsigned char threshold;
    unsigned char reserved[10];
} SataSMARTThreshold;

typedef struct AttributeData{
    SataSMARTAttribute attribute;
    SataSMARTThreshold threshold;
} AttributeData;

typedef struct SmartData{
    unsigned char smartVersion[2];
    AttributeData attributes[SATA_SMART_ATTRIBUTES_COUNT];
} SmartData;

/*
@brief Эта функция загружает полученные данные команды SMART READ DATA в структуру smartData
@param[out] smartData указатель на структуру SmartData, в которую требуется записать информацию из буфера
@param[in] data_buffer буфер, в котором храняться данные, полученные в результате выполнения SMART READ DATA
*/
void loadSataSMARTAttributes(SmartData* smartData, unsigned char data_buffer[SMART_DATA_SIZE_BYTES]);

/*
@brief Эта функция загружает полученные данные команды SMART READ THRESHOLD в структуру smartData
@param[out] smartData указатель на структуру SmartData, в которую требуется записать информацию из буфера
@param[in] data_buffer буфер, в котором храняться данные, полученные в результате выполнения SMART READ THRESHOLD
*/
void loadSataSMARTThresholds(SmartData* smartData, unsigned char data_buffer[SMART_DATA_SIZE_BYTES]);