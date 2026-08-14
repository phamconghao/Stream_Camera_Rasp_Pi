# Roadmap

> File này là nguồn thông tin **chính thức, nằm trong repo**, phản ánh đúng trạng thái code thật tại
> thời điểm cập nhật gần nhất. Nếu bạn thấy trạng thái ở nơi khác (khung chat khác, ghi chú cá nhân...)
> khác với file này — **file này mới là đúng**, vì mọi dòng dưới đây đều được đối chiếu trực tiếp với
> source code và build/test thật, không dựa trên trí nhớ hay suy đoán.
>
> Cập nhật lần cuối: dựa trên commit `31cdaf7` (nhánh `csi_camera_streaming`) — Phase 20 (RTSP Server) hoàn thành cả 6/6 bước trong phiên làm việc này.

## Lưu ý về Phase 1-11

Phase 1-11 (Nghiên cứu V4L2, Capture 1 frame, Continuous capture, HTTP MJPEG server,
Producer-Consumer, Zero-copy queue, Shared frame, Multi-client streaming, Đo latency, Logging,
Buffer ownership) **không tồn tại trong repo này**. Không có HTTP server, không có MJPEG, không có
multi-client fan-out ở bất kỳ đâu trong source code. Đây rất có thể là một project/bài tập riêng biệt
đã làm trước đó (dùng V4L2 thuần + MJPEG), khác hẳn kiến trúc CSI + H.264 + RTP hiện tại. Roadmap dưới
đây chỉ tính từ Phase 12 — điểm mà repo `Stream_Camera_Rasp_Pi` thực sự bắt đầu.

## Trạng thái các phase

| Phase | Nội dung | Trạng thái | Bằng chứng |
|---|---|---|---|
| 12 | CSI Camera + H.264 hardware encoder | ✅ Done | `capture/camera_capture.*`, `encoder/bcm2835_encoder.*` — build thật, verified |
| 13 | Annex-B (NAL parsing) | ✅ Done | `parser/h264_nal_parser.*` — iterator API, unit tested |
| 14 | RTP packetizer | ✅ Done | `rtp/rtp_packetizer.*` — Single NAL + FU-A (RFC 6184 §5.6, §5.8) |
| 15 | H.264 over RTP | ✅ Done | `rtp/rtp_packetizer_thread.*` — verified end-to-end qua UDP thật |
| 16 | RTP receiver + hardware decoder | ✅ **Done** | `main_receiver.cpp`, `network/udp_receiver*`, `decoder/bcm2835_decoder.*` — executable `camera_receiver` build sạch, test end-to-end thật |
| 17 | Jitter buffer | ✅ **Done** | `rtp/rtp_jitter_buffer.*` — ring buffer theo sequence, xử lý wraparound, phát hiện mất gói qua timeout, tự resync |
| 18 | Packet loss handling | ✅ **Done** | Phát hiện (jitter buffer) + discard access unit hỏng + **phục hồi chủ động**: `network/control_channel.*` gửi yêu cầu force-keyframe (rate-limited) tới `network/control_listener_thread.*` (sender), verified dưới ~17% packet loss thật |
| — | *(mở rộng ngoài roadmap gốc)* Adaptive bitrate | ✅ Done | `loss_reporter_thread` tính loss rate → `control_channel` báo cáo → sender áp `bcm2835_encoder_set_bitrate()` theo 3 tier (2Mbps/1Mbps/500kbps) |
| — | *(mở rộng)* Monitoring dashboard | ✅ Done | `dashboard.html` + `sender_stats.json`/`receiver_stats.json` (bitrate, loss%, fps, tổng keyframe request) |
| — | *(mở rộng)* Dashcam-style recording | ✅ Done | `writer/circular_h264_writer.*` — xoay segment theo thời lượng, tự xóa segment cũ khi vượt hạn mức |
| 19 | RTCP (RFC 3550 Receiver/Sender Reports) | ✅ **Done** | `rtp/rtcp_packet.*` (SR/RR wire format thật theo RFC 3550), `rtp/rtcp_sender_thread.*` (sender, SR multiplex chung port RTP data - RFC 5761 rtcp-mux), `rtp/rtcp_receiver_stats.*` + `rtp/rtcp_receiver_thread.*` (receiver, RR multiplex chung control channel) — verified end-to-end qua UDP thật: SR/RR trao đổi đúng, jitter tính đúng công thức RFC 3550 §6.4.1 tăng dần theo thời gian thực (0→703→2714), packet/octet count chính xác |
| 20 | RTSP Server (session negotiation, SDP, multi-client fan-out) | ✅ **Done (6/6 bước)** | **Bước 1-3** (đã verify qua RTSP client thật ở phiên trước: TCP server port 8554 + parser RTSP/1.0 + dispatch 5 method, `RtspSessionRegistry` giới hạn 5 session + orphan reaper 60s, `PipelineController` lazy-start ref-counted). **Bước 4** — RTP fan-out: `network/udp_sender.*` đổi từ 1 đích cố định sang tập đích theo `session_id` (mutex-guarded map), `handle_play()`/`handle_teardown()` (`rtsp/rtsp_server.*`) add/remove đích khi session PLAYING/kết thúc, reaper cũng dọn đích cho session mồ côi. **Bước 5** — SDP thật: `parser/sps_pps_cache.*` (cache SPS/PPS mới nhất từ `rtp_packetizer_thread`) + `common/base64.*` (RFC 4648 encoder tự viết) → `handle_describe()` sinh `a=fmtp:96 profile-level-id=...;sprop-parameter-sets=...` thật thay vì placeholder; xử lý case DESCRIBE đến trước PLAY bằng cách mượn tạm pipeline (poll tối đa 1s, trả `503` nếu vẫn chưa có SPS/PPS). **Bước 6** — dọn code chết: xóa `writer/h264_writer.*` (không ai gọi) + `h264_split_nals()` (chỉ được gọi bởi file vừa xóa) + `rtsp_session_registry_get_playing()` (mồ côi từ sau bước 4) + include thừa. Build sạch `-Wall -Wextra -Wpedantic -Werror` cho cả `camera_app`/`camera_receiver`, patch verify `git apply --check` clean trên clone sạch nhiều lần. ⚠️ **Bước 4-6 mới build-verified, CHƯA test qua RTSP client thật trên hardware** (khác bước 1-3) — cụ thể chưa xác nhận: nhiều client đồng thời cùng nhận đúng RTP, VLC/ffplay decode được từ SDP thật, hành vi reaper dọn đích khi client mất kết nối đột ngột |
| 21 | Adaptive bitrate | ✅ Đã làm sớm hơn dự kiến | Xem dòng "mở rộng" ở trên — hoàn thành trước khi tới lượt trong roadmap gốc |
| 22 | WebRTC | ⏳ Chưa làm | Có hướng thay thế tạm thời: xem trực tiếp qua trình duyệt bằng cầu nối HLS (`bridge.sh`, dùng ffmpeg, không phải WebRTC thật) |
| 23 | Security | ⏳ Chưa làm | Không có mã hóa/xác thực — UDP plaintext, không SRTP |
| 24 | Performance optimization | ⏳ Chưa làm | Có vài ghi chú giới hạn hiệu năng đã biết (xem phần dưới), chưa có đợt tối ưu riêng |

## Việc cần làm tiếp theo (theo thứ tự đề xuất)

1. **Test Phase 20 bước 4-6 trên hardware thật** — trước khi coi Phase 20 đóng hẳn: chạy `camera_app` trên Pi, test ≥2 client RTSP đồng thời (VLC/ffplay) xác nhận cả 2 cùng nhận đúng video và không ảnh hưởng nhau khi 1 client TEARDOWN; xác nhận SDP thật (`sprop-parameter-sets`) decode được ngay từ đầu; test kịch bản client mất kết nối đột ngột (không gửi TEARDOWN) xem reaper có dọn đích RTP đúng sau ~60s không.
2. **WebRTC (Phase 22)** — hạ tầng lớn tiếp theo còn thiếu sau khi Phase 20 xong. Hiện có hướng thay thế tạm: xem qua trình duyệt bằng cầu nối HLS (`bridge.sh`, dùng ffmpeg, không phải WebRTC thật).
3. **Audio (ý tưởng mở rộng)** — nhân bản kiến trúc pool/queue/thread hiện có cho luồng audio (ALSA capture → Opus encode → RTP SSRC riêng), cần bàn kỹ về đồng bộ lip-sync trước khi code.
4. **Security (Phase 23)** — SRTP hoặc ít nhất xác thực đơn giản cho control channel (hiện ai cũng gửi được lệnh force-keyframe/đổi bitrate tới sender nếu biết đúng port), và cho chính RTSP server (hiện không có auth, ai biết IP:8554 cũng SETUP/PLAY được).

## Giới hạn/nợ kỹ thuật đã biết (chưa chặn chức năng, nhưng đáng lưu ý)

- `bcm2835_encoder_encode_frame()`/`bcm2835_decoder_decode_frame()` luôn dùng buffer OUTPUT index cố định thay vì luân phiên — an toàn vì gọi đồng bộ (QBUF ngay sau DQBUF), nhưng chặn khả năng pipeline hóa song song nhiều frame.
- `control_channel`/`control_listener_thread` không có xác thực — bất kỳ ai gửi đúng gói UDP tới đúng port đều có thể ép sender đổi bitrate/tạo keyframe liên tục (không phải lỗ hổng nghiêm trọng ở quy mô LAN cá nhân, nhưng cần khắc phục trước khi expose ra Internet).
- Dashboard (`dashboard.html`) yêu cầu chạy HTTP server riêng ở đúng thư mục chứa `sender_stats.json`/`receiver_stats.json` — chưa có cách xem gộp cả 2 phía trên cùng 1 trang do sender/receiver thường là 2 máy khác nhau.
- `frame_pool.cpp`/`frame_queue.cpp` (thiết kế pool/queue generic cũ) vẫn còn trong `CMakeLists.txt` dù không được gọi ở đâu — an toàn để xóa khi thuận tiện.
- `rtsp/rtsp_server.cpp`'s `PLACEHOLDER_SERVER_RTP_PORT`/`PLACEHOLDER_SERVER_RTCP_PORT` (SETUP response's `Transport` header) là giá trị advertise cố định, KHÔNG phản ánh cổng nguồn UDP thật (từ Phase 20 bước 4, `udp_sender.cpp` dùng 1 socket dùng chung, cổng do OS tự cấp) — vô hại với hầu hết client (chỉ dùng để hiển thị/đoán cổng RTCP), nhưng nếu có client thực sự dựa vào giá trị này thì cần plumbing cổng thật ra từ `udp_sender.cpp`.
