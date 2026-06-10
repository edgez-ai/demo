local result = {}

local SERIAL_NUM = 0x00

local CMD_GET_VERSION = 0x11
local CMD_RESET = 0x26
local CMD_SET_DOWNSIZE = 0x31
local CMD_FBUF_CTRL = 0x36
local CMD_GET_FBUF_LEN = 0x34
local CMD_READ_FBUF = 0x32

local FBUF_STOP_FRAME = 0x00
local FBUF_RESUME_FRAME = 0x02

local READ_CHUNK_SIZE = 128
local MAX_FRAME_SIZE = 500000

local cam_baud = 115200
local cam_action = "capture"
local cam_output =  "capture.jpg"
local cam_reset = false
local cam_quiet =  false

local log_cfg = { quiet = cam_quiet }



local function camera_log(msg)
  util_log(log_cfg, "VC0706", msg)
end

local function strip_trailing_nulls_and_newlines(s)
  return tostring(s or ""):gsub("[%z\r\n]+$", "")
end

local function build_command(cmd, args)
  args = args or ""
  return string.char(0x56, SERIAL_NUM, cmd & 0xFF, #args & 0xFF) .. args
end

local function send_command(cmd, args, label)
  local packet = build_command(cmd, args)

  local ok, err = rs485_reset_rx_cursor()
  if not ok then
    return false, "failed to reset rx cursor: " .. tostring(err)
  end

  ok, err = rs485_write(packet)
  if not ok then
    return false, "failed to write command payload: " .. tostring(err)
  end

  return true
end

local function read_response(timeout_seconds, max_len, expected_len)
  local timeout = tonumber(timeout_seconds) or 2.0
  local max_bytes = tonumber(max_len) or 256
  local wanted = expected_len and tonumber(expected_len) or nil
  if wanted and wanted > max_bytes then
    wanted = max_bytes
  end

  local data = ""
  local no_data_count = 0
  local deadline = os.clock() + timeout

  while #data < max_bytes and os.clock() < deadline do
    local chunk = rs485_read_chunk()
    if chunk and #chunk > 0 then
      data = data .. chunk
      no_data_count = 0
      if wanted and #data >= wanted then
        break
      end
    else
      no_data_count = no_data_count + 1
      if #data > 0 and no_data_count >= 30 then
        break
      end
      rs485_sleep(0.05)
    end
  end

  if #data > max_bytes then
    data = data:sub(1, max_bytes)
  end

  return data
end

local function drain_buffer(timeout_seconds)
  local timeout = tonumber(timeout_seconds) or 0.3
  local drained = 0
  local deadline = os.clock() + timeout
  while os.clock() < deadline do
    local chunk = rs485_read_chunk()
    if chunk and #chunk > 0 then
      drained = drained + #chunk
    else
      rs485_sleep(0.05)
    end
  end
  if drained > 0 then
    camera_log("Drained " .. tostring(drained) .. " stray bytes")
  end
  return drained
end

local function verify_response(response, cmd)
  if not response or #response < 4 then
    return false
  end
  return string.byte(response, 1) == 0x76
    and string.byte(response, 2) == SERIAL_NUM
    and string.byte(response, 3) == (cmd & 0xFF)
    and string.byte(response, 4) == 0x00
end

local function get_version()
  drain_buffer(0.1)

  local ok, err = send_command(CMD_GET_VERSION, "", "GET_VERSION")
  if not ok then
    return false, err
  end

  local response = read_response(2.0, 256)
  if #response >= 5 and verify_response(response, CMD_GET_VERSION) then
    local version = strip_trailing_nulls_and_newlines(response:sub(6))
    if version ~= "" then
      print("Camera version: " .. version)
      return true, version
    end
  end

  local ascii = response:gsub("[^%g%s]", "")
  if ascii:find("Version", 1, true) or ascii:find("PTC", 1, true) or ascii:find("VC0706", 1, true) then
    ascii = strip_trailing_nulls_and_newlines(ascii)
    print("Camera info (ASCII):\n" .. ascii)
    return true, ascii
  end

  if #response > 0 then
    print("Unknown response (hex): " .. util_bytes_to_hex(response))
  else
    print("No response from camera")
  end
  return false, "failed to read camera version"
end

local function stop_frame()
  local ok, err = send_command(CMD_FBUF_CTRL, string.char(FBUF_STOP_FRAME), "STOP_FRAME")
  if not ok then
    return false, err
  end

  local response = read_response(2.0, 64)
  if #response >= 5 and verify_response(response, CMD_FBUF_CTRL) then
    return true
  end
  return #response > 0, "failed to stop frame"
end

local function resume_frame()
  local ok, err = send_command(CMD_FBUF_CTRL, string.char(FBUF_RESUME_FRAME), "RESUME_FRAME")
  if not ok then
    return false, err
  end

  local response = read_response(1.0, 64)
  if #response >= 5 and verify_response(response, CMD_FBUF_CTRL) then
    return true
  end
  return #response > 0, "failed to resume frame"
end

local function get_frame_buffer_length()
  local ok, err = send_command(CMD_GET_FBUF_LEN, string.char(0x00), "GET_FBUF_LEN")
  if not ok then
    return 0, err
  end

  local response = read_response(2.0, 64)
  if #response >= 9 and verify_response(response, CMD_GET_FBUF_LEN) then
    local b6 = string.byte(response, 6)
    local b7 = string.byte(response, 7)
    local b8 = string.byte(response, 8)
    local b9 = string.byte(response, 9)
    local length = ((b6 << 24) | (b7 << 16) | (b8 << 8) | b9)
    camera_log("Frame buffer length: " .. tostring(length) .. " bytes")
    return length
  end

  return 0, "invalid frame length response"
end

local function build_read_fbuf_args(offset, chunk_size)
  return string.char(
    0x00, 0x0A,
    (offset >> 24) & 0xFF,
    (offset >> 16) & 0xFF,
    (offset >> 8) & 0xFF,
    offset & 0xFF,
    (chunk_size >> 24) & 0xFF,
    (chunk_size >> 16) & 0xFF,
    (chunk_size >> 8) & 0xFF,
    chunk_size & 0xFF,
    0x00,
    0xFF
  )
end

local function read_frame_buffer_to_global(length, max_retries)
  local total = tonumber(length) or 0
  local retries = tonumber(max_retries) or 3

  if total <= 0 or total > MAX_FRAME_SIZE then
    return nil, "invalid frame length: " .. tostring(total)
  end

  if type(util_init_global_buffer) ~= "function" or type(util_append_global_buffer) ~= "function" then
    return nil, "util global buffer helpers are not available"
  end

  local init_ok, init_err = util_init_global_buffer()
  if not init_ok then
    return nil, "failed to init global buffer: " .. tostring(init_err)
  end

  local offset = 0

  while offset < total do
    local chunk_size = math.min(READ_CHUNK_SIZE, total - offset)
    local args = build_read_fbuf_args(offset, chunk_size)

    local response = ""
    for attempt = 1, retries do
      local ok = send_command(CMD_READ_FBUF, args, string.format("READ_FBUF@%d#%d", offset, attempt))
      if ok then
        response = read_response(4.0, chunk_size + 10, chunk_size + 10)
        if #response >= (5 + chunk_size) and verify_response(response, CMD_READ_FBUF) then
          break
        end
      end
      camera_log(string.format("Retry chunk offset %d attempt %d/%d, got %d bytes", offset, attempt, retries, #response))
    end

    if #response < 10 then
      return nil, "short read response at offset " .. tostring(offset) .. ": " .. tostring(#response) .. " bytes"
    end

    if not verify_response(response, CMD_READ_FBUF) then
      return nil, "invalid read-fbuf response header at offset " .. tostring(offset)
    end

    local payload_start = 6
    local payload_end = payload_start + chunk_size - 1
    if #response < payload_end then
      return nil, "chunk too short at offset " .. tostring(offset)
    end

    local payload = response:sub(payload_start, payload_end)
    local append_ok, append_err = util_append_global_buffer(payload)
    if not append_ok then
      return nil, "failed to append payload at offset " .. tostring(offset) .. ": " .. tostring(append_err)
    end

    payload = nil
    response = nil
    if collectgarbage then
      collectgarbage("step", 200)
    end

    offset = offset + chunk_size

    local progress = math.floor((offset * 100) / total)
    io.write(string.format("\rRead progress: %d%% (%d/%d)", progress, offset, total))
    io.flush()
  end

  print("")
  return true
end

local function reset_camera()
  local ok, err = send_command(CMD_RESET, "", "RESET")
  if not ok then
    return false, err
  end
  local response = read_response(3.0, 64)
  if #response >= 4 then
    return true
  end
  return false, "no reset response"
end

local function set_resolution()
  local args = string.char(0x04, 0x01, 0x00, 0x19, 0x11)
  local ok, err = send_command(CMD_SET_DOWNSIZE, args, "SET_RESOLUTION")
  if not ok then
    return false, err
  end

  local response = read_response(2.0, 64)
  if #response >= 5 then
    local ack = response:sub(1, 5)
    local expected = string.char(0x76, SERIAL_NUM, CMD_SET_DOWNSIZE, 0x01, 0x00)
    if ack == expected then
      return true
    end
  end

  if #response >= 4 and verify_response(response, CMD_SET_DOWNSIZE) then
    return true
  end

  return false, "resolution command not acknowledged"
end

local function capture_image()
  local ok, err = stop_frame()
  if not ok then
    return nil, err
  end

  local frame_len = 0
  for _ = 1, 3 do
    drain_buffer(0.1)
    frame_len = get_frame_buffer_length()
    if frame_len > 0 then
      break
    end
  end

  if frame_len <= 0 then
    resume_frame()
    return nil, "failed to get frame length"
  end

  local ok, read_err = read_frame_buffer_to_global(frame_len, 3)
  resume_frame()
  if not ok then
    return nil, read_err
  end
  return frame_len
end

local function run_action()
  local ok, err = rs485_connect(cam_baud)
  if not ok then
    return false, "failed to open rs485: " .. tostring(err)
  end

  rs485_sleep(3.0)

  local success, msg, extra
  if cam_reset then
    local reset_ok, reset_err = reset_camera()
    if not reset_ok then
      rs485_safe_close()
      return false, "camera reset failed: " .. tostring(reset_err)
    end
  end

  if cam_action == "version" then
    local version_ok, version_or_err = get_version()
    success = version_ok
    msg = version_or_err
  elseif cam_action == "set-resolution" then
    local set_ok, set_err = set_resolution()
    success = set_ok
    msg = set_ok and "resolution acknowledged" or set_err
  elseif cam_action == "capture" then
    local set_ok, set_err = set_resolution()
    if not set_ok then
      rs485_safe_close()
      return false, "failed to set resolution before capture: " .. tostring(set_err)
    end

    local captured_len, cap_err = capture_image()
    if not captured_len then
      rs485_safe_close()
      return false, "capture failed: " .. tostring(cap_err)
    end

    if type(util_write_global_buffer_at) ~= "function" then
      rs485_safe_close()
      return false, "util_write_global_buffer_at is not available"
    end

    local marker = "hello world"
    local write_ok, write_err = util_write_global_buffer_at(captured_len, marker)
    if not write_ok then
      rs485_safe_close()
      return false, "failed to write marker at end position: " .. tostring(write_err)
    end

    success = true
    msg = "capture buffered"
    extra = {
      output = cam_output,
      bytes = captured_len,
      persist_buffer = true,
    }
  else
    rs485_safe_close()
    return false, "unsupported action: " .. tostring(cam_action)
  end

  rs485_safe_close()
  if not success then
    return false, msg
  end
  return true, msg, extra
end

local ok, message, extra = run_action()
if not ok then
  error(message)
end

table.insert(result, {
  action = cam_action,
  status = "ok",
  message = tostring(message or "success"),
  output = extra and extra.output or nil,
  bytes = extra and extra.bytes or nil,
  persist_buffer = extra and extra.persist_buffer or nil,
})

return result
