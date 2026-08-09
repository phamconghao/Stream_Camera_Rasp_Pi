# Roadmap

> File này là nguồn thông tin **chính thức, nằm trong repo**, phản ánh đúng trạng thái code thật tại
> thời điểm cập nhật gần nhất. Nếu bạn thấy trạng thái ở nơi khác (khung chat khác, ghi chú cá nhân...)
> khác với file này — **file này mới là đúng**, vì mọi dòng dưới đây đều được đối chiếu trực tiếp với
> source code và build/test thật, không dựa trên trí nhớ hay suy đoán.
>
> Cập nhật lần cuối: dựa trên commit `aeec0a1` (nhánh `csi_camera_streaming`) + Phase 19 (RTCP) hoàn thành trong phiên làm việc này.

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
| 20 | RTSP Server (session negotiation, SDP, multi-client fan-out) | 🔄 Đang triển khai (bước 1-3/6 xong) | ✅ Bước 1 (`rtsp/rtsp_message.*` + `rtsp/rtsp_server.*` — TCP server thật port 8554, parser RTSP/1.0, dispatch đủ 5 method) + Bước 2 (`rtsp/rtsp_session_registry.*` — giới hạn 5 session, orphan reaper 60s) + **Bước 3** (`rtsp/pipeline_controller.*` — lazy-start pipeline theo ref_count, chỉ chạy camera/encoder khi có client PLAY, tự dừng khi client cuối TEARDOWN hoặc bị reaper dọn do crash) đều **đã verify qua RTSP client thật**: handshake đủ 5 method đúng, giới hạn 5 session đúng `453`, session lạ đúng `454`, và test mock camera xác nhận đúng `camera_capture_start`/`stop` gọi đúng **1 lần duy nhất** dù có nhiều client, PLAY trùng lặp, hay SETUP không PLAY. ⏳ Bước 4-6 chưa làm: fan-out RTP tới nhiều session (`udp_sender_thread` vẫn gửi 1 đích cố định), sinh SDP từ SPS/PPS thật (hiện SDP là placeholder), dọn code chết còn sót |
| 21 | Adaptive bitrate | ✅ Đã làm sớm hơn dự kiến | Xem dòng "mở rộng" ở trên — hoàn thành trước khi tới lượt trong roadmap gốc |
| 22 | WebRTC | ⏳ Chưa làm | Có hướng thay thế tạm thời: xem trực tiếp qua trình duyệt bằng cầu nối HLS (`bridge.sh`, dùng ffmpeg, không phải WebRTC thật) |
| 23 | Security | ⏳ Chưa làm | Không có mã hóa/xác thực — UDP plaintext, không SRTP |
| 24 | Performance optimization | ⏳ Chưa làm | Có vài ghi chú giới hạn hiệu năng đã biết (xem phần dưới), chưa có đợt tối ưu riêng |

## Việc cần làm tiếp theo (theo thứ tự đề xuất)

1. **RTSP Server (Phase 20)** — hạ tầng lớn nhất còn thiếu. Thiết kế đã bàn trước đó:
   - Port `8554`, path `/stream`, tối đa 5 client đồng thời
   - Method subset: `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY`, `TEARDOWN`
   - `RtspSessionRegistry` để fan-out động nhiều client (thay cho `dest_ip`/`dest_port` cố định hiện tại)
   - SDP sinh từ SPS/PPS đã cache
   - `PipelineController` (`ensure_running()`/`release()` bằng `ref_count_`) để lazy-start pipeline khi có client đầu tiên
   - Session state machine `INIT → READY → PLAYING`, timeout dọn session mồ côi
2. **Audio (ý tưởng mở rộng)** — nhân bản kiến trúc pool/queue/thread hiện có cho luồng audio (ALSA capture → Opus encode → RTP SSRC riêng), cần bàn kỹ về đồng bộ lip-sync trước khi code.
3. **Security (Phase 23)** — SRTP hoặc ít nhất xác thực đơn giản cho control channel (hiện ai cũng gửi được lệnh force-keyframe/đổi bitrate tới sender nếu biết đúng port).

## Giới hạn/nợ kỹ thuật đã biết (chưa chặn chức năng, nhưng đáng lưu ý)

- `bcm2835_encoder_encode_frame()`/`bcm2835_decoder_decode_frame()` luôn dùng buffer OUTPUT index cố định thay vì luân phiên — an toàn vì gọi đồng bộ (QBUF ngay sau DQBUF), nhưng chặn khả năng pipeline hóa song song nhiều frame.
- `control_channel`/`control_listener_thread` không có xác thực — bất kỳ ai gửi đúng gói UDP tới đúng port đều có thể ép sender đổi bitrate/tạo keyframe liên tục (không phải lỗ hổng nghiêm trọng ở quy mô LAN cá nhân, nhưng cần khắc phục trước khi expose ra Internet).
- Dashboard (`dashboard.html`) yêu cầu chạy HTTP server riêng ở đúng thư mục chứa `sender_stats.json`/`receiver_stats.json` — chưa có cách xem gộp cả 2 phía trên cùng 1 trang do sender/receiver thường là 2 máy khác nhau.
- `frame_pool.cpp`/`frame_queue.cpp` (thiết kế pool/queue generic cũ) vẫn còn trong `CMakeLists.txt` dù không được gọi ở đâu — an toàn để xóa khi thuận tiện.
