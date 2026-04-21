import asyncio
import struct
import json
import os
from database.database import AsyncSessionLocal
from database.repository import VideoEvidenceRepository

async def handle_custom_tcp_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    client_addr = writer.get_extra_info('peername')
    print(f"🔗 [TCP 8100] 클라이언트 접속: {client_addr}")
    
    try:
        while True:
            header_bytes = await reader.readexactly(20)
            if not header_bytes:
                break
            
            message_id = int.from_bytes(header_bytes[0:2], byteorder='big')
            print(f"📥 [TCP 8100] 패킷 수신 - Message ID: {message_id}")
            
            if message_id == 310:
                _, cam_id, _, start_time, end_time = struct.unpack('>HBBQQ', header_bytes)
                print(f"📥 [요청 수신] 리스트 조회 (Camera: {cam_id}, {start_time} ~ {end_time})")

                async with AsyncSessionLocal() as session:
                    repo = VideoEvidenceRepository(session)
                    rows = await repo.get_videos_by_time_range(cam_id, start_time, end_time)

                payload_list = [
                    {
                        "event_id": row.event_id, 
                        "camera_id": row.camera_id, 
                        "timestamp": row.timestamp_ms
                    } for row in rows
                ]
                payload_json = json.dumps(payload_list).encode('utf-8')
                payload_size = len(payload_json)

                status_code = 0x00 if rows else 0x01
                response_header = struct.pack('>HB13sI', 311, status_code, b'\x00' * 13, payload_size)

                writer.write(response_header + payload_json)
                await writer.drain()
                print(f"📤 [응답 전송] 311 리스트 데이터 (Status: {status_code}, Payload: {payload_size} bytes)")
                
            elif message_id == 302:
                _, cam_id, evt_type, evt_id, req_timestamp = struct.unpack('>HBBQQ', header_bytes)
                print(f"📥 [요청 수신] 영상 URL 조회 (Event ID: {evt_id})")

                async with AsyncSessionLocal() as session:
                    repo = VideoEvidenceRepository(session)
                    file_path = await repo.get_video_by_event_id(evt_id)

                if file_path:
                    status_code = 0x00
                    server_ip = os.getenv("SERVER_HOST_IP", "127.0.0.1")
                    video_url = f"http://{server_ip}:8000{file_path}"
                    payload_bytes = video_url.encode('utf-8')
                else:
                    status_code = 0x01
                    payload_bytes = b"Video file not found"

                payload_size = len(payload_bytes)
                response_header = struct.pack('>HB13sI', 303, status_code, b'\x00' * 13, payload_size)

                writer.write(response_header + payload_bytes)
                await writer.drain()
                print(f"📤 [응답 전송] 303 영상 URL (Status: {status_code}, Payload: {payload_bytes.decode('utf-8')})")
                
            else:
                print(f"⚠️ [TCP 8100] 알 수 없는 Message ID: {message_id}")
            
    except asyncio.IncompleteReadError:
        print(f"⚠️ [TCP 8100] 클라이언트 {client_addr} 연결 끊김 (EOF)")
    except Exception as e:
        print(f"❌ [TCP 8100] 통신 오류: {e}")
    finally:
        writer.close()
        await writer.wait_closed()
        print(f"🔌 [TCP 8100] 클라이언트 연결 종료: {client_addr}")
