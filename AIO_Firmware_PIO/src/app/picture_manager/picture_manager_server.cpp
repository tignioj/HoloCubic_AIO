#include "picture_manager_server.h"
#include "Arduino.h"
#include "common.h"
#include "picture_common.h"
#include <WebServer.h>
#include <SD.h>
#include <SPIFFS.h>
// #include <SimpleFTPServer.h>
// FtpServer ftpServer;
// 创建WebServer对象，监听81端口
static WebServer *picture_manager_server = nullptr;

// 全局变量用于文件上传
static File uploadFile;
static String currentUploadFilename;

// 设置CORS头
void setCorsHeaders() {
    // picture_manager_server->sendHeader("Access-Control-Allow-Origin", "*");
    // picture_manager_server->sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    // picture_manager_server->sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// 处理OPTIONS请求（CORS预检）
void handleOptions() {
    setCorsHeaders();
    picture_manager_server->send(200);
}

// 上传图片处理
void handlePictureFileUpload() {
    HTTPUpload& upload = picture_manager_server->upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        // 检查目录是否存在
        if (!SD.exists(IMAGE_FOLDER_PATH)) {
            SD.mkdir(IMAGE_FOLDER_PATH);
        }
        
        currentUploadFilename = upload.filename;
        String filepath = String(IMAGE_FOLDER_PATH) + "/" + currentUploadFilename;
        Serial.printf("开始上传文件: %s\n", currentUploadFilename.c_str());
        
        // 打开文件
        uploadFile = SD.open(filepath, FILE_WRITE);
        if (!uploadFile) {
            Serial.printf("无法打开文件: %s\n", filepath.c_str());
        }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            uploadFile.write(upload.buf, upload.currentSize);
        }
    }
    else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            Serial.printf("上传完成: %s, 大小: %u\n", currentUploadFilename.c_str(), upload.totalSize);
            
            // 更新索引
            String newName = "\n" + currentUploadFilename;
            const char *fn = newName.c_str();
            // TODO 去重
            if(tf.appendFile(IMAGE_INDEX_PATH, fn)) {
                Serial.printf("成功更新图片索引-追加一行%s\n", fn);
            } else {
                Serial.printf("添加文件名称为%s图片索引失败！\n", fn);
            }
        }
    }
}

// 处理上传完成后的响应
void handleUploadComplete() {
    setCorsHeaders();
    picture_manager_server->send(200, "text/plain", "Upload complete");
}

// 删除图片处理
void handleFileDelete() {
    if (picture_manager_server->hasArg("filename")) {
        String filename = picture_manager_server->arg("filename");
        String filepath = String(IMAGE_FOLDER_PATH) + "/" + filename;
        
        bool exists_file = SD.exists(filepath);
        Serial.printf("%s图片存在吗? %s\n", filepath.c_str(), exists_file ? "是" : "否");
        
        if(!exists_file) {
            setCorsHeaders();
            picture_manager_server->send(404, "text/plain", "no such file!");
            return;
        }
        
        SD.remove(filepath);
        
        // 更新索引
        if(safe_delete_line_from_index_file(IMAGE_INDEX_PATH, filename.c_str())) {
            Serial.printf("成功更新索引-删除指定文件名称:%s\n", filename.c_str());
        }
        
        setCorsHeaders();
        picture_manager_server->send(200, "text/plain", "file deleted");
    } else {
        setCorsHeaders();
        picture_manager_server->send(400, "text/plain", "Missing filename parameter");
    }
}

/**
 * 递归删除指定目录下的所有文件和子目录
 * @param path 要清空的目录路径
 */
void deleteDirectoryContents(const char * path) {
    Serial.printf("清空目录: %s\n", path);
    
    // 1. 打开目录
    File root = SD.open(path);
    if(!root){
        Serial.println("打开目录失败");
        return;
    }
    if(!root.isDirectory()){
        Serial.println("错误：提供的路径不是目录");
        return;
    }

    // 2. 遍历目录中的所有项
    File file = root.openNextFile();
    while(file){
        // String filePath = String(path) + "/" + String(file.name()); // 构建完整路径
        // Serial.print("path:");
        // Serial.println(path);
        // Serial.print("filename:");
        Serial.println(file.name());

        // 3. 判断是文件还是子目录
        if(file.isDirectory()){
            // 如果是子目录，递归调用自身先清空子目录
            deleteDirectoryContents(file.name());
            // 子目录清空后，删除这个空目录
            if(!SD.rmdir(file.name())){
                Serial.printf("删除空目录失败: %s\n", file.name());
            } else {
                Serial.printf("已删除空目录: %s\n", file.name());
            }
        } else {
            // 如果是文件，直接删除
            if(!SD.remove(file.name())){
                Serial.printf("删除文件失败: %s\n", file.name());
            } else {
                Serial.printf("已删除文件: %s\n", file.name());
            }
        }
        file.close();
        file = root.openNextFile(); // 继续处理下一项
    }
    // 清空索引
    tf.writeFile(IMAGE_INDEX_PATH, "");
    Serial.println("清空图片索引内容");
    file.close();
}

// 删除所有图片处理
void handleFileDeleteAll() {
    String filepath = IMAGE_FOLDER_PATH;
    bool exists_file = SD.exists(filepath);
    Serial.printf("%s图片路径存在吗? %s\n", filepath.c_str(), exists_file ? "是" : "否");
    
    if (exists_file) {
        deleteDirectoryContents(filepath.c_str());
        setCorsHeaders();
        picture_manager_server->send(200, "text/plain", "all image deleted");
    } else {
        setCorsHeaders();
        picture_manager_server->send(404, "text/plain", "no such folder!");
    }
}

// 获取图片处理
void handleFileGet() {
    if (picture_manager_server->hasArg("filename")) {
        String filename = picture_manager_server->arg("filename");
        String filepath = String("/image/") + filename;
        
        if (SD.exists(filepath)) {
            // 设置正确的Content-Type
            picture_manager_server->sendHeader("Content-Type", "image/jpeg");
            // 发送文件
            File file = SD.open(filepath, FILE_READ);
            if (file) {
                picture_manager_server->streamFile(file, "image/jpeg");
                file.close();
            } else {
                picture_manager_server->send(500, "text/plain", "Failed to open file");
            }
        } else { 
            picture_manager_server->send(404, "text/plain", "文件不存在");
        }
    } else {
        picture_manager_server->send(400, "text/plain", "Missing filename parameter");
    }
}

// 更新图片索引处理
void handleUpdateIndex() {
    Serial.println("开始更新图片索引...");
    bool success = create_files_index(IMAGE_FOLDER_PATH, IMAGE_INDEX_PATH);
    
    if (success) {
        Serial.println("图片索引更新完成 - 成功");
        setCorsHeaders();
        picture_manager_server->send(200, "text/plain", "Scan completed successfully");
    } else {
        Serial.println("图片索引更新完成 - 失败");
        setCorsHeaders();
        picture_manager_server->send(500, "text/plain", "Scan failed");
    }
}

// 列出图片处理
void handleFileList() {
    // 注意不要写成/image/,而是/image，区别在于最后一个斜杠。
    File index_file = tf.open(IMAGE_INDEX_PATH);
    if (!index_file) {
        Serial.println("IMAGE_INDEX_PATH索引文件不存在");
        setCorsHeaders();
        picture_manager_server->send(404, "text/plain", "Index file not found");
        return;
    }
    
    String jsonArray = "["; // 开始构建JSON数组
    bool firstLine = true; // 标记是否为第一行，用于控制逗号
    while (index_file.available()) {
        String fileName = index_file.readStringUntil('\n'); // 逐行读取
        fileName.trim(); // 去除换行符和空格

        if (fileName.length() > 0) {
            if (!firstLine) {
                jsonArray += ","; // 非首行元素前添加逗号
            }
            firstLine = false;
            jsonArray += "\"" + fileName + "\""; // 为文件名添加双引号
        }
    }
    index_file.close();
    jsonArray += "]"; // 闭合JSON数组
    Serial.print("图片列表:");
    Serial.println(jsonArray);
    
    setCorsHeaders();
    picture_manager_server->send(200, "application/json", jsonArray);
}

// 处理404错误
void handleNotFound() {
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += picture_manager_server->uri();
    message += "\nMethod: ";
    message += (picture_manager_server->method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += picture_manager_server->args();
    message += "\n";
    
    for (uint8_t i = 0; i < picture_manager_server->args(); i++) {
        message += " " + picture_manager_server->argName(i) + ": " + picture_manager_server->arg(i) + "\n";
    }
    
    picture_manager_server->send(404, "text/plain", message);
}



void start_picture_manager_server() {
    if(init_picture_manager_server()) {
        Serial.println("Picture manager server initialized.");
    } else {
        Serial.println("Picture manager server already running.");
        return;
    }
    
    // 配置路由
    // 注意：ESP32自带的WebServer不支持像AsyncWebServer那样的serveStatic方法
    // 我们需要单独处理静态文件，或者使用SPIFFS/SD卡的文件服务
    

    // picture_manager_server->serveStatic("/assets", SD, "/www/assets/");
    // picture_manager_server->serveStatic("/", SD, "/www/index.html");
    // picture_manager_server->serveStatic("/assets/index.js", SD, "/www/assets/index.js");
    // picture_manager_server->serveStatic("/assets/index.js.gz", SD, "/www/assets/index.js.gz");
    // picture_manager_server->serveStatic("/assets/index.css", SD, "/www/assets/index.css");
    // picture_manager_server->serveStatic("/assets/index.css.gz", SD, "/www/assets/index.css.gz");

    picture_manager_server->serveStatic("/image", SD, "/image/");
    picture_manager_server->serveStatic("/assets", SD, "/www/assets/");
    picture_manager_server->serveStatic("/favicon.ico", SD, "/www/favicon.ico");
    picture_manager_server->serveStatic("/", SD, "/www/index.html");
    
    // 处理OPTIONS请求（CORS预检）
    picture_manager_server->on("/api/image/upload", HTTP_OPTIONS, handleOptions);
    picture_manager_server->on("/api/image/delete", HTTP_OPTIONS, handleOptions);
    picture_manager_server->on("/api/image/delete_all", HTTP_OPTIONS, handleOptions);
    picture_manager_server->on("/api/image/get", HTTP_OPTIONS, handleOptions);
    picture_manager_server->on("/api/image/list", HTTP_OPTIONS, handleOptions);
    picture_manager_server->on("/api/image/update_index", HTTP_OPTIONS, handleOptions);
    
    // picture_manager_server->serveStatic("/", SD, "/www/");
    // 上传接口
    picture_manager_server->on("/api/image/upload", HTTP_POST, handleUploadComplete, handlePictureFileUpload);
    
    // 删除接口
    picture_manager_server->on("/api/image/delete", HTTP_GET, handleFileDelete);
    
    // 删除所有接口
    picture_manager_server->on("/api/image/delete_all", HTTP_GET, handleFileDeleteAll);
    
    // 获取图片接口
    picture_manager_server->on("/api/image/get", HTTP_GET, handleFileGet);
    
    // 列表接口
    picture_manager_server->on("/api/image/list", HTTP_GET, handleFileList);
    
    // 更新索引
    picture_manager_server->on("/api/image/update_index", HTTP_GET, handleUpdateIndex);
    
    // 处理404错误
    picture_manager_server->onNotFound(handleNotFound);

    // 静态文件服务,注意路由顺序，如果把先注册根路径/，那么后面注册的所有/api/路径都会被视为静态文件处理，所以要放在最后
    // picture_manager_server->serveStatic("/image", SD, "/image/");
    // picture_manager_server->serveStatic("/assets", SD, "/www/assets/");
    // picture_manager_server->serveStatic("/", SD, "/www/index.html");
    // 开始服务器
    picture_manager_server->begin();
    Serial.println("HTTP server started on port 81");
}

void stop_picture_manager_server() {
    if(nullptr != picture_manager_server) {
        Serial.println("停止图片服务器");
        picture_manager_server->stop();
        picture_manager_server->close();
        delete picture_manager_server;
        picture_manager_server = nullptr;
    }
    if(uploadFile) {
        uploadFile.close();
    }
}

bool init_picture_manager_server() {
    if(nullptr == picture_manager_server) {
        Serial.println("Creating server...");
        picture_manager_server = new WebServer(81);
        return true;
    } else {
        Serial.println("Server already exists!");
        return false;
    }
}

void picture_manager_server_handle() {
    // ESP32自带的WebServer需要在loop中调用handleClient()
    if (picture_manager_server != nullptr) {
        picture_manager_server->handleClient();
    }
}

