#pragma once

#include <stdint.h>

#include <string>

/* ==========================================================================
 *  照片 -> 服务器视觉分析接口
 *
 *  地址和 token 不是配在固件里的：服务器在 MCP 握手的 initialize 请求里用
 *  params.capabilities.vision = {url, token} 下发（服务端
 *  core/providers/tools/device_mcp/mcp_handler.py 的 send_mcp_initialize_message），
 *  mcp_server.cc 收下存在 McpServer 里，这里取出来用。所以没握过手就没有地址，
 *  此时上传直接跳过。
 *
 *  上传内容 = multipart/form-data：
 *
 *      --boundary
 *      Content-Disposition: form-data; name="question"
 *      <问题>
 *      --boundary
 *      Content-Disposition: form-data; name="file"; filename="camera.jpg"
 *      Content-Type: image/jpeg
 *      <JPEG 字节>
 *      --boundary--
 *
 *  字段顺序不能换：服务端 vision_handler.py 是 reader.next() 两次按顺序取的，
 *  不认字段名。头部带 Device-Id / Client-Id / Authorization: Bearer <token>，
 *  服务端会拿 Device-Id 和 token 里带的设备号比对（不一致直接 401 "设备ID与token不匹配"），
 *  所以 Device-Id 必须和 WebSocket 握手时发的是同一个值（都是 MAC）。
 * ========================================================================== */

/* 服务器有没有下发过视觉分析地址。 */
bool xiaowei_vision_available();

/* 把一帧 RGB565 传上去，返回服务器给出的画面描述（纯文本）。失败返回空串并记日志。

   pixels：RGB565，字节序同相机/LVGL 那份（见 gc2145.cc 的字节交换那一段，
   所以格式常量是 V4L2_PIX_FMT_RGB565 而不是 RGB565X）。
   stride：**必须**等于 w*2 —— image_to_jpeg 只吃连续内存，相册里存的正好是连续的。

   会编码 + 走一次 HTTP，耗时跟模型出结果有关（几百 ms 到十几秒），同步跑完。
   调用方别在网络/音频回调任务里调，也不要在 LVGL 锁里调。 */
std::string xiaowei_vision_explain(const uint8_t* pixels, int w, int h, int stride,
                                   const std::string& question);
