#include "sd_card.h"
#include "SD_MMC.h"
#include <string.h>
#include "common.h"
#define TF_VFS_IS_NULL(RET)                           \
    if (NULL == tf_vfs)                               \
    {                                                 \
        Serial.println("[Sys SD Card] Mount Failed"); \
        return RET;                                   \
    }

int photo_file_num = 0;
char file_name_list[DIR_FILE_NUM][DIR_FILE_NAME_MAX_LEN];

static fs::FS *tf_vfs = NULL;

/*
 * get file basename
 */
static const char *get_file_basename(const char *path)
{
    // 获取最后一个'/'所在的下标
    const char *ret = path;
    for (const char *cur = path; *cur != 0; ++cur)
    {
        if (*cur == '/')
        {
            ret = cur + 1;
        }
    }
    return ret;
}


// 声明一个互斥锁句柄（全局变量）
SemaphoreHandle_t xSDCardMutex;
// 从索引文件中加载目录，例如 "/movie/movie.txt"
// 其中索引的内容是文件名称
File_Info *load_files_from_index(const char* index_path, const char* index_folder)
{
    File index_file = tf.open(index_path);
    if (!index_file) {
        Serial.println("Index file not found, will scan directory");
        return NULL;
    }
    
    // 创建头节点（表示文件夹）
    File_Info *head_file = (File_Info *)malloc(sizeof(File_Info));
    if (!head_file) {
        Serial.println("Memory allocation failed for head file");
        index_file.close();
        return NULL;
    }
    
    head_file->file_type = FILE_TYPE_FOLDER;
    head_file->file_name = strdup(index_folder);
    head_file->front_node = NULL;
    head_file->next_node = NULL;
    
    File_Info *tail_file = head_file;
    char line[255]; 
    int file_count = 0;
    bool allocated_failed = false;
    while (index_file.available()) {
        int bytesRead = index_file.readBytesUntil('\n', line, sizeof(line)-1);
        if (bytesRead > 0) {
            line[bytesRead] = '\0';
            // 去除换行符
            if (bytesRead > 0 && line[bytesRead-1] == '\r') {
                line[bytesRead-1] = '\0';
                bytesRead--;
            }
            
            // 跳过空行
            if (bytesRead == 0) continue;
            
            // 验证文件实际存在
            // char full_path[MAX_FILENAME_LENGTH];
            // snprintf(full_path, sizeof(full_path), "%s/%s", MOVIE_PATH, line);
            // 不要验证了，播放的时候再验证，不然每一个都验证相当于全盘扫面
            // TODO 播放的时候验证文件是否存在，不存在则从索引里面删除
            // if (!SD.exists(full_path)) {
            //     Serial.printf("File in index not found: %s\n", full_path);
            //     continue;
            // }
            
            // 创建文件节点
            File_Info *new_file = (File_Info *)malloc(sizeof(File_Info));
            if (!new_file) {
                Serial.println("Memory allocation failed for file info");
                release_file_info(head_file); // 这个函数会把头节点也清理掉
                allocated_failed = true;
                // TODO 这里直接break掉了，之前分配的内存没有释放，有内存泄漏风险
                break;
            }
            
            new_file->file_name = strdup(line);
            new_file->file_type = FILE_TYPE_FILE;
            
            // 添加到链表
            tail_file->next_node = new_file;
            new_file->front_node = tail_file;
            new_file->next_node = NULL;
            tail_file = new_file;
            
            file_count++;
        }
    }
    
    index_file.close();

    if(allocated_failed) return NULL;

    // 将链表设置为循环（与listDir保持一致）
    if (head_file->next_node) {
        // 将最后一个节点的next指向第一个文件节点
        tail_file->next_node = head_file->next_node;
        // 将第一个文件节点的front指向最后一个节点
        head_file->next_node->front_node = tail_file;
    } else {
        // 如果没有文件，头节点自循环
        head_file->next_node = head_file;
        head_file->front_node = head_file;
    }

    Serial.printf("Loaded %d files from index\n", file_count);
    return head_file;
}


// 纯更新索引，不创建链表，为了节省内存
bool create_files_index(const char* path, const char* index_path) {
    TF_VFS_IS_NULL(false)

    Serial.printf("Listing directory: %s\n", path);

    File root = tf_vfs->open(path);
    if (!root)
    {
        Serial.println("Failed to open directory");
        return false;
    }
    if (!root.isDirectory())
    {
        Serial.println("Not a directory");
        return false;
    }
    // 创建索引文件
    File index_file = tf.open(index_path, FILE_WRITE);
    if (!index_file) {
        Serial.println("Failed to create index file");
        return false;
    }

   File file = root.openNextFile();
    while (file)
    {
        const char *file_base_name = get_file_basename(file.name());
        int filename_len = strlen(file_base_name);
        if (filename_len > FILENAME_MAX_LEN - 10)
        {
            Serial.println("Filename is too long.");
            file.close();
            file = root.openNextFile();
            continue;
        }
        // 跳过索引本身
        if (strcmp(file_base_name, get_file_basename(index_path)) == 0)
        {
            file.close();
            file = root.openNextFile();
            continue;
        }
        // 写入索引文件
        index_file.println(file_base_name); // 只写入文件名，不包含路径
        file.close();
        file = root.openNextFile();
    }
    index_file.close();
    root.close();
    Serial.println("Index file created successfully.");
    return true;    
}
 



// TODO防止多线程访问
// 更新索引-删除索引中的指定文件名称
bool delete_line_from_index_file(const char* filepath, const char* filenameToDelete) {
    if(!xSDCardMutex) {
        Serial.println("互斥锁为空，无法操作！");
        return false;
    }
     File sourceFile = SD.open(filepath, FILE_READ);
    if (!sourceFile) {
    Serial.println("错误：无法打开源文件进行读取。");
    return false;
  }

  // 创建一个临时文件
  String tempFilePath = String(filepath) + ".tmp";
  File tempFile = SD.open(tempFilePath.c_str(), FILE_WRITE);
  if (!tempFile) {
    Serial.println("错误：无法创建临时文件。");
    sourceFile.close();
    return false;
  }

  String line;
  bool lineFound = false;
  
  // 逐行读取源文件
  while (sourceFile.available()) {
    line = sourceFile.readStringUntil('\n');
    line.trim(); // 去除换行符和空格

    // 如果这一行不是要删除的文件名，则写入临时文件
    if (line != String(filenameToDelete)) {
      tempFile.println(line);
    } else {
      lineFound = true; // 标记找到了要删除的行
    }
  }

  sourceFile.close();
  tempFile.close();

  // 删除原文件，并将临时文件重命名为原文件
  if (!SD.remove(filepath)) {
    Serial.println("错误：删除原文件失败。");
    SD.remove(tempFilePath.c_str()); // 清理临时文件
    return false;
  }
  if (!SD.rename(tempFilePath.c_str(), filepath)) {
    Serial.println("错误：重命名临时文件失败。");
    return false;
  }

  if (lineFound) {
    Serial.println("指定行已成功删除。");
  } else {
    Serial.println("未找到指定的文件名。");
  }
  return true;
}


// 使用互斥锁保护你的文件操作函数
bool safe_delete_line_from_index_file(const char* filepath, const char* filenameToDelete) {
    if(!xSDCardMutex) {
        Serial.println("互斥锁为空，无法操作！");
        return false;
    }
  // 请求获取互斥锁，等待最大时间为portMAX_DELAY（一直等）
  if (xSemaphoreTake(xSDCardMutex, portMAX_DELAY) == pdTRUE) {
    
    // 成功获取到锁，执行受保护的文件操作
    bool result = delete_line_from_index_file(filepath, filenameToDelete); // 调用你原来的函数
    
    // 操作完成后，必须释放锁！
    xSemaphoreGive(xSDCardMutex);
    
    return result; // 返回操作结果
  }
  // 如果获取锁失败，返回false
  return false;
}


bool safe_append_to_index_file(const char* filepath, const char* filenameToAppend) {
    TF_VFS_IS_NULL(false)

    File readFile = tf_vfs->open(filepath, FILE_READ);
    if (readFile)
    {
        String fileContent;
        while (readFile.available())fileContent = readFile.readString();
        readFile.close();
        
        // 检查内容是否已存在
        if (fileContent.indexOf(filenameToAppend) != -1)
        {
            Serial.println("Content already exists in file, skipping write.");
            return true; // 内容已存在，返回成功但不写入
        }
    }
readFile.close();
  return false;
}


// ==================== 文件链表释放函数 ====================
 void release_file_info(File_Info *head)
{

    if (!head) return;
    
    // 检查是否是空链表（只有头节点）
    if (head->next_node == head) {
        if (head->file_name) { 
            free(head->file_name);
        }
        free(head);
        return;
    }
    
    // 断开循环链表，变成单向链表
    // 获取第一个文件节点
    File_Info *first_file = head->next_node;
    // 获取最后一个文件节点（通过第一个节点的front_node）
    File_Info *last_file = first_file->front_node;
    
    // 断开循环：将最后一个节点的next置为NULL
    if (last_file) {
        last_file->next_node = NULL;
    }
    
    // 现在可以安全遍历单向链表
    File_Info *current = first_file;
    while (current) {
        File_Info *next = current->next_node;
        
        if (current->file_name) {
            free(current->file_name);
            current->file_name = NULL;
        }
        
        free(current);
        current = next;
    }
    
    // 最后释放头节点
    if (head->file_name) {
        free(head->file_name);
        head->file_name = NULL;
    }
    free(head);
}


void join_path(char *dst_path, const char *pre_path, const char *rear_path)
{
    while (*pre_path != 0)
    {
        *dst_path = *pre_path;
        ++dst_path;
        ++pre_path;
    }
    if (*(pre_path - 1) != '/')
    {
        *dst_path = '/';
        ++dst_path;
    }

    if (*rear_path == '/')
    {
        ++rear_path;
    }
    while (*rear_path != 0)
    {
        *dst_path = *rear_path;
        ++dst_path;
        ++rear_path;
    }
    *dst_path = 0;
}

bool SdCard::init()
{
    tf_vfs = NULL;
    SPIClass *sd_spi = new SPIClass(HSPI);          // another SPI
    sd_spi->begin(SD_SCK, SD_MISO, SD_MOSI, SD_SS); // Replace default HSPI pins
    if (!SD.begin(SD_SS, *sd_spi, 80000000))        // SD-Card SS pin is 15
    {
        Serial.println("Card Mount Failed");
        return false;
    }
    tf_vfs = &SD;
    xSDCardMutex = xSemaphoreCreateMutex(); // 创建互斥锁
    if (xSDCardMutex == NULL) {
        Serial.println("互斥锁创建失败！");
        // while(1); // 死循环，系统无法启动
    }
    uint8_t cardType = SD.cardType();

    // 目前SD_MMC驱动与硬件引脚存在冲突
    // if(!SD_MMC.begin("/", true)){
    //     Serial.println("Card Mount Failed");
    //     return;
    // }
    // tf_vfs = &SD_MMC;
    // uint8_t cardType = SD_MMC.cardType();

    if (cardType == CARD_NONE)
    {
        Serial.println("No SD card attached");
        tf_vfs = NULL;
        SD.end();
        return false;
    }

    Serial.print("SD Card Type: ");
    if (cardType == CARD_MMC)
    {
        Serial.println("MMC");
    }
    else if (cardType == CARD_SD)
    {
        Serial.println("SDSC");
    }
    else if (cardType == CARD_SDHC)
    {
        Serial.println("SDHC");
    }
    else
    {
        Serial.println("UNKNOWN");
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);
    return true;
}

bool SdCard::isMounted() const
{
    return tf_vfs != NULL;
}

void SdCard::listDir(const char *dirname, uint8_t levels)
{
    TF_VFS_IS_NULL()

    Serial.printf("Listing directory: %s\n", dirname);
    photo_file_num = 0;

    File root = tf_vfs->open(dirname);
    if (!root)
    {
        Serial.println("Failed to open directory");
        return;
    }
    if (!root.isDirectory())
    {
        Serial.println("Not a directory");
        return;
    }

    int dir_len = strlen(dirname) + 1;

    File file = root.openNextFile();
    while (file && photo_file_num < DIR_FILE_NUM)
    {
        if (file.isDirectory())
        {
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if (levels)
            {
                listDir(file.name(), levels - 1);
            }
        }
        else
        {
            Serial.print("  FILE: ");
            // 只取文件名 保存到file_name_list中
            strncpy(file_name_list[photo_file_num], file.name() + dir_len, DIR_FILE_NAME_MAX_LEN - 1);
            file_name_list[photo_file_num][strlen(file_name_list[photo_file_num]) - 4] = 0;

            char file_name[FILENAME_MAX_LEN] = {0};
            sprintf(file_name, "%s/%s.bin", dirname, file_name_list[photo_file_num]);
            Serial.print(file_name);
            ++photo_file_num;
            Serial.print("  SIZE: ");
            Serial.println(file.size());
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    Serial.println(photo_file_num);
}

File_Info *SdCard::listDir(const char *dirname)
{
    TF_VFS_IS_NULL(NULL)

    Serial.printf("Listing directory: %s\n", dirname);

    File root = tf_vfs->open(dirname);
    if (!root)
    {
        Serial.println("Failed to open directory");
        return NULL;
    }
    if (!root.isDirectory())
    {
        Serial.println("Not a directory");
        return NULL;
    }

    // 头节点的创建（头节点用来记录此文件夹）
    File_Info *head_file = (File_Info *)malloc(sizeof(File_Info));
    if (!head_file) return NULL;
    
    head_file->file_type = FILE_TYPE_FOLDER;
    head_file->file_name = strdup(dirname); // 使用strdup简化
    if (!head_file->file_name) {
        free(head_file);
        return NULL;
    }
    head_file->front_node = NULL;
    head_file->next_node = NULL;

    File_Info *file_node = head_file;

    File file = root.openNextFile();
    while (file)
    {
        const char *file_base_name = get_file_basename(file.name());
        int filename_len = strlen(file_base_name);
        if (filename_len > FILENAME_MAX_LEN - 10)
        {
            Serial.println("Filename is too long.");
            file = root.openNextFile();
            continue;
        }

        // 创建新节点
        File_Info *new_node = (File_Info *)malloc(sizeof(File_Info));
        if (!new_node) break;
        
        new_node->front_node = file_node;
        new_node->next_node = NULL;
        
        // 使用strdup分配并复制文件名
        new_node->file_name = strdup(file_base_name);
        if (!new_node->file_name) {
            free(new_node);
            break;
        }

        // 设置文件类型
        if (file.isDirectory())
        {
            new_node->file_type = FILE_TYPE_FOLDER;
        }
        else
        {
            new_node->file_type = FILE_TYPE_FILE;
        }

        // 连接到链表
        file_node->next_node = new_node;
        file_node = new_node;

        // 打印信息
        char tmp_file_name[FILENAME_MAX_LEN] = {0};
        join_path(tmp_file_name, dirname, file_node->file_name);
        
        if (file_node->file_type == FILE_TYPE_FOLDER)
        {
            Serial.print("  DIR : ");
            Serial.println(tmp_file_name);
        }
        else
        {
            Serial.print("  FILE: ");
            Serial.print(tmp_file_name);
            Serial.print("  SIZE: ");
            Serial.println(file.size());
        }

        file = root.openNextFile();
    }

    // 处理循环链表连接
    if (head_file->next_node)
    {
        file_node->next_node = head_file->next_node;
        head_file->next_node->front_node = file_node;
    }
    
    return head_file;
}

void SdCard::createDir(const char *path)
{
    TF_VFS_IS_NULL()

    Serial.printf("Creating Dir: %s\n", path);
    if (tf_vfs->mkdir(path))
    {
        Serial.println("Dir created");
    }
    else
    {
        Serial.println("mkdir failed");
    }
}

void SdCard::removeDir(const char *path)
{
    TF_VFS_IS_NULL()

    Serial.printf("Removing Dir: %s\n", path);
    if (tf_vfs->rmdir(path))
    {
        Serial.println("Dir removed");
    }
    else
    {
        Serial.println("rmdir failed");
    }
}

void SdCard::readFile(const char *path)
{
    TF_VFS_IS_NULL()

    Serial.printf("Reading file: %s\n", path);

    File file = tf_vfs->open(path);
    if (!file)
    {
        Serial.println("Failed to open file for reading");
        return;
    }

    Serial.print("Read from file: ");
    while (file.available())
    {
        Serial.write(file.read());
    }
    file.close();
}

String SdCard::readFileLine(const char *path, int num)
{
    TF_VFS_IS_NULL("")

    Serial.printf("Reading file: %s line: %d\n", path, num);

    File file = tf_vfs->open(path);
    if (!file)
    {
        return ("Failed to open file for reading");
    }

    char *p = buf;
    while (file.available())
    {
        char c = file.read();
        if (c == '\n')
        {
            num--;
            if (num == 0)
            {
                *(p++) = '\0';
                String s(buf);
                s.trim();
                return s;
            }
        }
        else if (num == 1)
        {
            *(p++) = c;
        }
    }
    file.close();

    return String("error parameter!");
}

void SdCard::writeFile(const char *path, const char *info)
{
    TF_VFS_IS_NULL()

    Serial.printf("Writing file: %s\n", path);

    File file = tf_vfs->open(path, FILE_WRITE);
    if (!file)
    {
        Serial.println("Failed to open file for writing");
        return;
    }
    if (file.println(info))
    {
        Serial.println("Write succ");
    }
    else
    {
        Serial.println("Write failed");
    }
    file.close();
}

File SdCard::open(const String &path, const char *mode)
{
    if (tf_vfs == NULL)
    {
        Serial.println("[Sys SD Card] Mount Failed");
        return File();
    }

    return tf_vfs->open(path, mode);
}

bool SdCard::appendFile(const char *path, const char *contentToWrite)
{
    TF_VFS_IS_NULL(false);
    bool success = false;

    Serial.printf("Appending '%s' to file: %s\n", contentToWrite, path);
    File file = tf_vfs->open(path, FILE_APPEND);
    if (!file)
    {
        Serial.println("Failed to open file for appending");
        return false;
    }
    if (file.print(contentToWrite))
    {

        Serial.printf("Message '%s' appended\n", contentToWrite);
        success = true;
    }
    else
    {
        Serial.println("Append failed");
    }
    file.close();
    return success;
}

void SdCard::renameFile(const char *path1, const char *path2)
{
    TF_VFS_IS_NULL()

    Serial.printf("Renaming file %s to %s\n", path1, path2);
    if (tf_vfs->rename(path1, path2))
    {
        Serial.println("File renamed");
    }
    else
    {
        Serial.println("Rename failed");
    }
}

boolean SdCard::deleteFile(const char *path)
{
    TF_VFS_IS_NULL(false)

    Serial.printf("Deleting file: %s\n", path);
    if (tf_vfs->remove(path))
    {
        Serial.println("File deleted");
        return true;
    }
    else
    {
        Serial.println("Delete failed");
    }
    return false;
}

boolean SdCard::deleteFile(const String &path)
{
    TF_VFS_IS_NULL(false)

    Serial.printf("Deleting file: %s\n", path);
    if (tf_vfs->remove(path))
    {
        Serial.println("File deleted");
        return true;
    }
    else
    {
        Serial.println("Delete failed");
    }
    return false;
}

void SdCard::readBinFromSd(const char *path, uint8_t *buf)
{
    TF_VFS_IS_NULL()

    File file = tf_vfs->open(path);
    size_t len = 0;
    if (file)
    {
        len = file.size();

        while (len)
        {
            size_t toRead = len;
            if (toRead > 512)
            {
                toRead = 512;
            }
            file.read(buf, toRead);
            len -= toRead;
        }

        file.close();
    }
    else
    {
        Serial.println("Failed to open file for reading");
    }
}

void SdCard::writeBinToSd(const char *path, uint8_t *buf)
{
    TF_VFS_IS_NULL()

    File file = tf_vfs->open(path, FILE_WRITE);
    if (!file)
    {
        Serial.println("Failed to open file for writing");
        return;
    }

    size_t i;
    for (i = 0; i < 2048; i++)
    {
        file.write(buf, 512);
    }
    file.close();
}

void SdCard::fileIO(const char *path)
{
    TF_VFS_IS_NULL()

    File file = tf_vfs->open(path);
    static uint8_t buf[512];
    size_t len = 0;
    uint32_t start = millis();
    uint32_t end = start;
    if (file)
    {
        len = file.size();
        size_t flen = len;
        start = millis();
        while (len)
        {
            size_t toRead = len;
            if (toRead > 512)
            {
                toRead = 512;
            }
            file.read(buf, toRead);
            len -= toRead;
        }
        end = millis() - start;
        Serial.printf("%u bytes read for %u ms\n", flen, end);
        file.close();
    }
    else
    {
        Serial.println("Failed to open file for reading");
    }

    file = tf_vfs->open(path, FILE_WRITE);
    if (!file)
    {
        Serial.println("Failed to open file for writing");
        return;
    }

    size_t i;
    start = millis();
    for (i = 0; i < 2048; i++)
    {
        file.write(buf, 512);
    }
    end = millis() - start;
    Serial.printf("%u bytes written for %u ms\n", 2048 * 512, end);
    file.close();
}
