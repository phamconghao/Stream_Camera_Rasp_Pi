# Postmortem: WebRTC video không hiển thị trên trình duyệt (màn hình đen)

> Ghi lại lịch sử debug 1 phiên làm việc dài (nhiều vòng lặp), từ lúc phát hiện lỗi
> đến lúc xác nhận fix xong. Mục đích: tránh lặp lại cùng một hướng điều tra sai
> nếu vấn đề tương tự xuất hiện lại, và ghi nhớ *tại sao* mỗi fix trung gian là cần
> thiết dù không phải root cause cuối cùng.

## Triệu chứng ban đầu

Mở `webrtc_test.html`, nhấn Connect + Send Offer: toàn bộ checklist báo xanh
(WebSocket, SDP offer/answer, ICE `connected`, DTLS `connected`, nhận được video
track qua `ontrack`) — nhưng thẻ `<video>` **luôn hiển thị màn hình đen**, không có
lỗi nào hiện ra ở console hay ở server.

Trong khi đó, RTSP (VLC/ffplay) xem luồng từ cùng con Pi, cùng camera, hoàn toàn
bình thường (chỉ có vấn đề màu sắc riêng biệt, không liên quan bug này).

## Vì sao khó tìm: WebRTC "connected" không có nghĩa là video sẽ chạy

`connectionState: connected` chỉ xác nhận ICE + DTLS-SRTP bắt tay xong — tầng vận
chuyển đã sẵn sàng. Nó **không** đảm bảo:
- Codec/profile/level đã khớp giữa 2 bên
- Encoder có tạo keyframe kịp thời cho viewer mới không
- Bitstream nhận được có thực sự decode được không

Mọi lớp phía trên (RTP depacketize, SRTP decrypt, H.264 decode) có thể fail hoàn
toàn âm thầm — trình duyệt không báo lỗi JS nào cho việc này, chỉ đơn giản là
`<video>` không bao giờ có frame để vẽ.

## Timeline các bug đã tìm và fix (theo thứ tự phát hiện)

### 1. Encoder xuất H.264 **High Profile** trong khi browser chỉ nhận payload type đã thoả thuận cho **Baseline**

**Nguyên nhân:** `webrtc_sdp.cpp` khi parse offer chỉ lấy **payload type H.264 đầu
tiên** tìm thấy có `packetization-mode=1` (ví dụ PT 102 = Baseline theo offer của
Chrome/Edge), rồi build answer với chính PT đó — nhưng `profile-level-id` trong
answer lại tính từ SPS *thật* của encoder, lúc đó đang là High Profile (driver
`bcm2835-codec` mặc định). Kết quả: answer nói "PT 102 = Baseline" nhưng bitstream
thật lại là High → decoder trình duyệt khởi tạo sai kỳ vọng, không parse được
CABAC/8x8-transform của High Profile.

**Fix:** Set `V4L2_CID_MPEG_VIDEO_H264_PROFILE = BASELINE` khi init encoder
(`bcm2835_encoder.cpp`).

**Vì sao chưa đủ:** Fix xong vẫn đen — chuyển sang bug #2.

### 2. Level H.264 vẫn là 4.0 trong khi offer chỉ khai 3.1

**Nguyên nhân:** Set profile Baseline không đổi level; driver vẫn giữ level mặc
định (4.0), trong khi PT 102 mà browser offer khai `profile-level-id=42001f`
(level 3.1) và answer không có `level-asymmetry-allowed=1`.

**Fix:** Set thêm `V4L2_CID_MPEG_VIDEO_H264_LEVEL = LEVEL_3_1`.

**Vì sao chưa đủ:** Vẫn đen. (Về sau xác nhận: level tự nó không phải nguyên nhân
chặn decode — chỉ là 1 điểm không khớp SDP hợp lý cần sửa cho đúng chuẩn, không
phải root cause.)

### 3. Không ép keyframe khi 1 WebRTC session mới sẵn sàng

**Nguyên nhân:** `dtls_handshake.cpp` khi session đạt "media ready" chỉ gọi
`webrtc_media_registry_add()` + `pipeline_controller_ensure_running()`, không hề
ép encoder tạo IDR mới cho riêng viewer đó. Viewer mới có thể join giữa GOP, chỉ
nhận P-frame không giải mã được cho tới khi GOP tự nhiên quay vòng hoặc trình
duyệt tự gửi PLI.

**Fix:** Gọi `bcm2835_encoder_force_keyframe()` ngay khi session ready, **trước**
khi mở cổng fan-out (`webrtc_media_registry_add()`), để gói đầu tiên viewer nhận
được luôn là gói của IDR vừa ép.

**Vì sao chưa đủ:** Vẫn đen — vì (xem bug #5) chính cái IDR "ép" ra đó cũng thiếu
SPS/PPS.

### 4. DTLS fingerprint verify luôn cứng SHA-256, không theo thuật toán offer khai báo

**Phát hiện khi nào:** Lúc dựng client Python độc lập (`aiortc`) để test ngoài
trình duyệt — `aiortc` mặc định ký cert bằng **SHA-512**, trong khi
`dtls_handshake.cpp` luôn gọi `dtls_cert_fingerprint_sha256()` bất kể
`a=fingerprint:<algo>` trong offer ghi gì. So sánh SHA-256 với giá trị SHA-512 →
luôn mismatch → session bị từ chối, không tạo SRTP.

**Lưu ý quan trọng:** Bug này **không phải nguyên nhân của vấn đề màn hình đen**
trên Chrome/Edge (2 trình duyệt đó luôn dùng SHA-256, không đụng bug này) — nó chỉ
là chướng ngại vật tình cờ chặn đường điều tra bằng `aiortc`. Nhưng vẫn là bug thật
cần sửa (sẽ chặn bất kỳ client WebRTC nào ký cert bằng thuật toán khác SHA-256).

**Fix:** Thêm `dtls_cert_fingerprint(cert, algo_name)` tổng quát (hỗ trợ sha-1/256/
384/512), dùng đúng `session->remote_fingerprint_algo` đã parse từ offer thay vì
hardcode SHA-256.

### 5. ROOT CAUSE THẬT SỰ: encoder chỉ phát SPS/PPS **một lần duy nhất** lúc khởi động

**Cách tìm ra:** Sau khi fix bug #4, dùng `aiortc` connect thành công, nhưng vẫn
**0/469 frame decode được**, lỗi ffmpeg: `avcodec_send_packet(): Invalid data`.
Patch sâu vào `JitterBuffer._remove_frame()` của `aiortc` để log chính xác những
packet nào được gom vào mỗi frame → xác nhận **client nhận đúng 100% byte server
gửi** (không mất gói, không lắp ráp sai). Đối chiếu ngược lại log server: frame đầu
tiên gửi cho session mới đi **thẳng vào NAL `IDR`, không có NAL `SPS`/`PPS` đứng
trước**.

**Nguyên nhân gốc:** `bcm2835-codec` (driver V4L2 M2M hardware encoder) theo mặc
định chỉ emit SPS/PPS **một lần duy nhất**, ngay sau khi encoder khởi tạo. Mọi IDR
sau đó (kể cả IDR bị ép bằng `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`, xem bug #3) chỉ
là 1 slice trần, không kèm SPS/PPS.

- **RTSP (VLC/ffmpeg) không bao giờ gặp vấn đề này** vì các client đó khởi tạo
  decoder từ `sprop-parameter-sets` nằm ngay trong SDP (`a=fmtp`), không cần đợi
  SPS/PPS in-band.
- **WebRTC (`aiortc` VÀ trình duyệt thật) không dùng `sprop-parameter-sets` để
  bootstrap** — decoder chỉ khởi tạo được từ SPS/PPS thực sự nằm trong luồng RTP.
  Bất kỳ viewer WebRTC nào kết nối **sau** lần phát SPS/PPS duy nhất đó (tức gần
  như luôn luôn, vì lần phát đó xảy ra lúc pipeline priming, trước khi viewer kịp
  connect) sẽ không bao giờ decode được **dù nhận đủ, nhận đúng, nhận liên tục**
  bao nhiêu IDR đi nữa.

**Fix:** Set `V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER = 1` khi init encoder — bắt
driver tự động chèn lại SPS/PPS trước **mỗi** IDR, không chỉ lần đầu.

**Kết quả verify:** `aiortc` decode thành công 462/462 frame (trước đó 0/469).
Test lại bằng trình duyệt thật (Chrome + Edge) trên laptop qua WiFi thật: video
hiển thị bình thường.

### Bug phụ (không liên quan tới màn hình đen, tiện tay sửa luôn)

- `webrtc_test.html`: thẻ `<div class="card">` bọc phần Video thiếu `</div>` đóng,
  khiến các card sau bị lồng nhầm vào bên trong — chỉ là lỗi layout, không ảnh
  hưởng chức năng.

## Kỹ thuật/công cụ debug đã dùng (theo thứ tự leo thang)

1. Đọc code + build thử trên chính con Pi (xác nhận app khởi động sạch).
2. So sánh SDP offer/answer thật giữa các lần test — phát hiện bug #1/#2.
3. `pc.getStats()` trong DevTools Console (Chrome/Edge) — thấy `packetsReceived`
   tăng đều nhưng `framesDecoded: 0`, `framesAssembledFromMultiplePackets: 0` →
   xác nhận bug nằm sau tầng transport, không phải ICE/DTLS/SRTP.
4. `edge://webrtc-internals`, `edge://media-internals` — không cho thêm thông tin
   hữu ích (WebRTC video track không đi qua pipeline mà `media-internals` theo
   dõi).
5. **Self-test SRTP round-trip ngay trong server**: tự mã hoá rồi tự giải mã lại
   bằng đúng key mà trình duyệt sẽ dùng (`server_write` key) — chứng minh crypto
   pipeline của server đúng 100%, tách hẳn nghi vấn "SRTP sai" ra khỏi danh sách.
6. **Dựng WebRTC client độc lập bằng Python (`aiortc`)**, không phụ thuộc trình
   duyệt — cho phép patch trực tiếp vào tận `RtpRouter.route_rtp()` và
   `JitterBuffer._remove_frame()` để quan sát chính xác từng packet, từng frame
   được lắp ráp thế nào. Đây là bước quyết định tìm ra root cause thật.
7. So sánh log server (NAL type nào được emit cho mỗi frame) với dữ liệu client
   nhận được → lộ ra frame thiếu SPS/PPS.

## Bài học rút ra

- **RTSP chạy được không có nghĩa là WebRTC sẽ chạy được**, kể cả khi dùng chung
  encoder/packetizer — 2 giao thức có kỳ vọng khác nhau về việc SPS/PPS phải nằm
  ở đâu (SDP vs in-band RTP).
- `connectionState: connected` / `iceConnectionState: connected` chỉ xác nhận
  tầng vận chuyển, không xác nhận được nội dung media có decode được hay không.
  Phải nhìn vào `getStats()`'s `framesDecoded`/`framesReceived` mới biết thật.
- Khi trình duyệt không đưa ra lỗi rõ ràng, dựng 1 client test độc lập (không phải
  Chrome/Edge) mà mình có toàn quyền patch/log là cách hiệu quả nhất để nhìn xuyên
  qua "hộp đen" của trình duyệt.
- Một V4L2 hardware encoder mặc định **không** tự lặp lại SPS/PPS trước mỗi IDR —
  đây là điều bất kỳ project WebRTC nào dùng encoder phần cứng cũng cần kiểm tra
  tường minh (`V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER`), không nên giả định driver
  tự làm đúng.
