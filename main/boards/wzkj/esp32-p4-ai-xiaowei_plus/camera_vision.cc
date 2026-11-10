#include "camera_vision.h"

#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_log.h>

#include "board.h"
#include "image_to_jpeg.h"
#include "mcp_server.h"
#include "system_info.h"

static const char* TAG = "VISION";

/* 服务器只按顺序取前两个 part，不认字段名，但名字还是写正常的。
   boundary 里的字符都安全（aiohttp 那边只当分隔符用）。 */
static const char* kBoundary = "----XIAOWEI_CAMERA_BOUNDARY";

/* 视觉分析是服务器拿大模型看图后写一段话，慢的时候十几秒。
   这里只等 HTTP 响应本身；服务端给 MCP 工具结果留了 30s（mcp_handler.py 的
   call_mcp_tool timeout=30），所以别比它更久，不然模型那边先超时了。 */
static const int kHttpTimeoutMs = 20000;

/* JPEG 质量。466x350 出图大约 20~35KB，远在服务端 5MB 上限之内。 */
static const uint8_t kJpegQuality = 80;

bool xiaowei_vision_available()
{
    std::string url, token;
    return McpServer::GetInstance().GetVisionEndpoint(url, token);
}

std::string xiaowei_vision_explain(const uint8_t* pixels, int w, int h, int stride,
                                   const std::string& question)
{
    if (!pixels || w <= 0 || h <= 0) return "";
    if (stride != w * 2) {
        /* image_to_jpeg 只吃连续内存，有行距就得先重排一份 —— 目前只有相机那
           一路会走到这儿，它的 stride 恒等于 w*2，真出现了说明调用方搞错了。 */
        ESP_LOGE(TAG, "stride %d != w*2 (%d)，不传了", stride, w * 2);
        return "";
    }

    std::string url, token;
    if (!McpServer::GetInstance().GetVisionEndpoint(url, token)) {
        ESP_LOGW(TAG, "服务器没下发视觉分析地址（MCP initialize 的 capabilities.vision），跳过上传");
        return "";
    }

    /* 编码。JPEG 缓冲由 image_to_jpeg 在 PSRAM 上分配，用完 free()。
       它内部还可能再开一份转换缓冲（RGB565 -> YUV），都在 PSRAM。 */
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    const bool encoded = image_to_jpeg(const_cast<uint8_t*>(pixels), (size_t)w * (size_t)h * 2,
                                       (uint16_t)w, (uint16_t)h, V4L2_PIX_FMT_RGB565,
                                       kJpegQuality, &jpeg, &jpeg_len);
    if (!encoded || jpeg == nullptr || jpeg_len == 0) {
        ESP_LOGE(TAG, "JPEG 编码失败（%dx%d）", w, h);
        if (jpeg) free(jpeg);
        return "";
    }

    /* 拼 multipart 请求体。整体走 SetContent：这个 Http 客户端在 Open() 里一次
       把请求头和 body 发出去，body 有值时会自己算 Content-Length。 */
    std::string body;
    body.reserve(jpeg_len + question.size() + 320);
    body += "--";
    body += kBoundary;
    body += "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n";
    body += question;
    body += "\r\n--";
    body += kBoundary;
    body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n";
    body.append(reinterpret_cast<const char*>(jpeg), jpeg_len);
    body += "\r\n--";
    body += kBoundary;
    body += "--\r\n";
    free(jpeg);

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "没网，传不了");
        return "";
    }
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "CreateHttp 失败");
        return "";
    }

    http->SetTimeout(kHttpTimeoutMs);
    /* Device-Id 必须和 WebSocket 握手时发的那个一模一样（websocket_protocol.cc
       也是 SystemInfo::GetMacAddress()）：服务端拿它跟 token 里的设备号比。 */
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid());
    http->SetHeader("Authorization", "Bearer " + token);
    http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + kBoundary);
    http->SetContent(std::move(body));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "连不上视觉分析接口：%s", url.c_str());
        return "";
    }
    const int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    if (status != 200) {
        ESP_LOGE(TAG, "视觉分析接口返回 %d：%s", status, response.c_str());
        return "";
    }

    /* 成功长这样：{"success":true,"action":"RESPONSE","response":"<描述>"}
       失败长这样：{"success":false,"message":"..."}（HTTP 也是 200） */
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "解析不了服务器返回：%s", response.c_str());
        return "";
    }
    std::string result;
    const cJSON* success = cJSON_GetObjectItem(root, "success");
    const cJSON* text = cJSON_GetObjectItem(root, "response");
    const cJSON* message = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsTrue(success) && cJSON_IsString(text) && text->valuestring) {
        result = text->valuestring;
    } else {
        ESP_LOGE(TAG, "视觉分析失败：%s",
                 (cJSON_IsString(message) && message->valuestring) ? message->valuestring : response.c_str());
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "上传 %dx%d JPEG %u 字节 -> %u 字节描述", w, h,
             (unsigned)jpeg_len, (unsigned)result.size());
    return result;
}
