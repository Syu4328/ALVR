#pragma once

#include <WinSock2.h>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include "threadtools.h"
#include "Logger.h"
#include "UdpSocket.h"
#include "Utils.h"
#include "Poller.h"
#include "ControlSocket.h"
#include "packet_types.h"

class Listener : public CThread {
public:

	Listener(std::string host, int port, std::string control_host, int control_port
		, std::function<void(std::string, std::string)> callback, std::function<void()> poseCallback)
		: m_bExiting(false)
		, m_Connected(false)
		, m_Streaming(false)
		{
		m_LastSeen = 0;
		m_CommandCallback = callback;
		m_PoseUpdatedCallback = poseCallback;
		memset(&m_TrackingInfo, 0, sizeof(m_TrackingInfo));
		InitializeCriticalSection(&m_CS);

		m_Settings.type = 4;
		m_Settings.enableTestMode = 0;
		m_Settings.suspend = 0;

		m_Poller.reset(new Poller());
		m_Socket.reset(new UdpSocket(host, port, m_Poller));
		m_ControlSocket.reset(new ControlSocket(control_host, control_port, m_Poller));

		m_UseUdp = true;
		m_Streaming = false;
	}

	~Listener() {
		DeleteCriticalSection(&m_CS);
	}

	void Run() override
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

		m_Socket->Startup();
		m_ControlSocket->Startup();
		
		while (!m_bExiting) {
			CheckTimeout();
			if (m_Poller->Do() <= 0) {
				continue;
			}

			sockaddr_in addr;
			int addrlen = sizeof(addr);
			char buf[2000];
			int len = sizeof(buf);
			if (m_Socket->Recv(buf, &len, &addr, addrlen)) {
				ProcessRecv(buf, len, &addr);
			}

			m_ControlSocket->Accept();
			std::vector<std::string> commands;
			if (m_ControlSocket->Recv(commands)) {
				for (auto it = commands.begin(); it != commands.end(); ++it) {
					std::string commandName, args;

					size_t split = it->find(" ");
					if (split != std::string::npos) {
						commandName = it->substr(0, split);
						args = it->substr(split + 1);
					}
					else {
						commandName = *it;
						args = "";
					}

					Log("Control Command: %s %s", commandName.c_str(), args.c_str());

					if (commandName == "EnableTestMode") {
						m_Settings.enableTestMode = atoi(args.c_str());
						SendChangeSettings();
						SendCommandResponse("Success\n");
					}
					else if (commandName == "Suspend") {
						m_Settings.suspend = atoi(args.c_str());
						SendChangeSettings();
						SendCommandResponse("Success\n");
					}
					else if (commandName == "GetRequests") {
						std::string str;
						for (auto it = m_Requests.begin(); it != m_Requests.end(); it++) {
							char buf[500];
							snprintf(buf, sizeof(buf), "%s %s\n"
								, AddrPortToStr(&it->address).c_str()
								, it->deviceName);
							str += buf;
						}
						SendCommandResponse(str.c_str());
					}
					else if (commandName == "Connect") {
						auto index = args.find(":");
						if (index == std::string::npos) {
							// Invalid format.
							SendCommandResponse("Fail\n");
						}
						else {
							std::string host = args.substr(0, index);
							int port = atoi(args.substr(index + 1).c_str());
							sockaddr_in addr;
							addr.sin_family = AF_INET;
							addr.sin_port = htons(port);
							inet_pton(addr.sin_family, host.c_str(), &addr.sin_addr);

							Log("Connected to %s:%d", host.c_str(), port);

							m_Socket->SetClientAddr(&addr);
							m_Connected = true;
							UpdateLastSeen();

							ConnectionMessage message = {};
							message.type = 6;
							m_Socket->Send((char *)&message, sizeof(message), 0);

							SendCommandResponse("Success\n");
						}
					}
					else {
						m_CommandCallback(commandName, args);
					}

				}
			}
		}
	}

	void Send(uint8_t *buf, int len, uint64_t presentationTime, uint64_t frameIndex) {
		uint8_t packetBuffer[2000];

		if (!m_Socket->IsClientValid()) {
			Log("Skip sending packet because client is not connected. Packet Length=%d FrameIndex=%llu", len, frameIndex);
			return;
		}
		if (!m_Streaming) {
			Log("Skip sending packet because streaming is off.");
			return;
		}
		Log("Sending %d bytes FrameIndex=%llu", len, frameIndex);

		int chunks = (len + PACKET_SIZE - 1) / PACKET_SIZE;
		for (int i = 0; i < chunks; i++) {
			int size = min(PACKET_SIZE, len - (i * PACKET_SIZE));
			int pos = 0;

			if (i == 0) {
				*(uint32_t *)packetBuffer = 1;
				pos += sizeof(uint32_t);
				*(uint32_t *)(packetBuffer + pos) = packetCounter;
				pos += sizeof(uint32_t);

				// Insert presentation time header in first packet.
				*(uint64_t *)(packetBuffer + pos) = presentationTime;
				pos += sizeof(uint64_t);
				*(uint64_t *)(packetBuffer + pos) = frameIndex;
				pos += sizeof(uint64_t);
			}else{
				*(uint32_t *)packetBuffer = 2;
				pos += sizeof(uint32_t);
				*(uint32_t *)(packetBuffer + pos) = packetCounter;
				pos += sizeof(uint32_t);
			}
			packetCounter++;

			memcpy(packetBuffer + pos, buf + i * PACKET_SIZE, size);
			pos += size;

			if (i == chunks - 1) {
				// Insert padding so that client can detect end of packet
				memcpy(packetBuffer + pos, "\x00\x00\x00\x02", 4);
				pos += 4;
			}
			int ret = m_Socket->Send((char *)packetBuffer, pos, frameIndex);

		}
	}

	void ProcessRecv(char *buf, int len, sockaddr_in *addr) {
		if (len < 4) {
			return;
		}
		int pos = 0;
		uint32_t type = *(uint32_t*)buf;

		Log("Received packet. Type=%d", type);
		if (type == 1 && len >= sizeof(HelloMessage)) {
			HelloMessage *message = (HelloMessage *)buf;
			SanitizeDeviceName(message->deviceName);

			Log("Hello Message: %s", message->deviceName);

			PushRequest(message, addr);
		}
		else if (type == 2 && len >= sizeof(TrackingInfo)) {
			if (!m_Connected || !m_Socket->IsLegitClient(addr)) {
				char str[100];
				inet_ntop(AF_INET, &addr->sin_addr, str, sizeof(str));
				Log("Recieved message from invalid address: %s:%d", str, htons(addr->sin_port));
				return;
			}
			UpdateLastSeen();

			EnterCriticalSection(&m_CS);
			m_TrackingInfo = *(TrackingInfo *)buf;
			LeaveCriticalSection(&m_CS);

			Log("got tracking info %d %f %f %f %f", (int)m_TrackingInfo.FrameIndex,
				m_TrackingInfo.HeadPose_Pose_Orientation.x,
				m_TrackingInfo.HeadPose_Pose_Orientation.y,
				m_TrackingInfo.HeadPose_Pose_Orientation.z,
				m_TrackingInfo.HeadPose_Pose_Orientation.w);
			m_PoseUpdatedCallback();
		}
		else if (type == 3 && len >= sizeof(TimeSync)) {
			if (!m_Connected || !m_Socket->IsLegitClient(addr)) {
				char str[100];
				inet_ntop(AF_INET, &addr->sin_addr, str, sizeof(str));
				Log("Recieved message from invalid address: %s:%d", str, htons(addr->sin_port));
				return;
			}
			UpdateLastSeen();

			TimeSync *timeSync = (TimeSync*)buf;
			uint64_t Current = GetTimestampUs();

			if (timeSync->mode == 0) {
				TimeSync sendBuf = *timeSync;
				sendBuf.mode = 1;
				sendBuf.serverTime = Current;
				m_Socket->Send((char *)&sendBuf, sizeof(sendBuf), 0);
			}
			else if (timeSync->mode == 2) {
				// Calclate RTT
				uint64_t RTT = Current - timeSync->serverTime;
				// Estimated difference between server and client clock
				uint64_t TimeDiff = Current - (timeSync->clientTime + RTT / 2);
				m_TimeDiff = TimeDiff;
				Log("TimeSync: server - client = %lld us RTT = %lld us", TimeDiff, RTT);
			}
		}
		else if (type == 7 && len >= sizeof(StreamControlMessage)) {
			if (!m_Connected || !m_Socket->IsLegitClient(addr)) {
				char str[100];
				inet_ntop(AF_INET, &addr->sin_addr, str, sizeof(str));
				Log("Recieved message from invalid address: %s:%d", str, htons(addr->sin_port));
				return;
			}
			StreamControlMessage *streamControl = (StreamControlMessage*)buf;

			if (streamControl->mode == 1) {
				Log("Stream control message: Start stream.");
				m_Streaming = true;
			}
			else if (streamControl->mode == 2) {
				Log("Stream control message: Stop stream.");
				m_Streaming = false;
			}
		}
	}

	void SendChangeSettings() {
		if (!m_Socket->IsClientValid()) {
			return;
		}
		m_Socket->Send((char *)&m_Settings, sizeof(m_Settings), 0);
	}

	void Stop()
	{
		m_bExiting = true;
		m_Socket->Shutdown();
		m_ControlSocket->Shutdown();
		Join();
	}

	bool HasValidTrackingInfo() const {
		return m_TrackingInfo.type == 2;
	}

	void GetTrackingInfo(TrackingInfo &info) {
		EnterCriticalSection(&m_CS);
		info = m_TrackingInfo;
		LeaveCriticalSection(&m_CS);
	}

	uint64_t clientToServerTime(uint64_t clientTime) const {
		return clientTime + m_TimeDiff;
	}

	uint64_t serverToClientTime(uint64_t serverTime) const {
		return serverTime - m_TimeDiff;
	}

	void SendCommandResponse(const char *commandResponse) {
		m_ControlSocket->SendCommandResponse(commandResponse);
	}

	void PushRequest(HelloMessage *message, sockaddr_in *addr) {
		for (auto it = m_Requests.begin(); it != m_Requests.end(); ++it) {
			if (it->address.sin_addr.S_un.S_addr == addr->sin_addr.S_un.S_addr && it->address.sin_port == addr->sin_port) {
				m_Requests.erase(it);
				break;
			}
		}
		Request request = {};
		request.address = *addr;
		memcpy(request.deviceName, message->deviceName, sizeof(request.deviceName));
		request.timestamp = GetTimestampUs();

		m_Requests.push_back(request);
		if (m_Requests.size() > 10) {
			m_Requests.pop_back();
		}
	}

	void SanitizeDeviceName(char deviceName[32]) {
		deviceName[31] = 0;
		auto len = strlen(deviceName);
		if (len != 31) {
			memset(deviceName + len, 0, 31 - len);
		}
		for (int i = 0; i < len; i++) {
			if (!isalnum(deviceName[i]) && deviceName[i] != '_' && deviceName[i] != '-') {
				deviceName[i] = '_';
			}
		}
	}

	std::string DumpConfig() {
		char buf[1000];
		
		sockaddr_in addr = {};
		if (m_Connected) {
			addr = m_Socket->GetClientAddr();
		}
		else {
			addr.sin_family = AF_INET;
		}
		char host[100];
		inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
		
		snprintf(buf, sizeof(buf)
			, "Connected %d\n"
			"Client %s:%d\n"
			"Streaming %d\n"
			, m_Connected ? 1 : 0
			, host, htons(addr.sin_port)
			, m_Streaming);

		return buf;
	}

	void CheckTimeout() {
		if (!m_Connected){
			return;
		}

		uint64_t Current = GetTimestampUs();

		if (Current - m_LastSeen > 60 * 1000 * 1000) {
			// idle for 60 seconcd
			// Invalidate client
			m_Socket->InvalidateClient();
			m_Connected = false;
			Log("Client timeout for idle");
		}
	}

	void UpdateLastSeen() {
		m_LastSeen = GetTimestampUs();
	}

private:
	bool m_bExiting;
	bool m_UseUdp;
	std::shared_ptr<Poller> m_Poller;
	std::shared_ptr<UdpSocket> m_Socket;
	std::shared_ptr<ControlSocket> m_ControlSocket;

	// Maximum SRT(or UDP) payload is PACKET_SIZE + 16
	static const int PACKET_SIZE = 1000;

	uint32_t packetCounter = 0;

	time_t m_LastSeen;
	std::function<void(std::string, std::string)> m_CommandCallback;
	std::function<void()> m_PoseUpdatedCallback;
	TrackingInfo m_TrackingInfo;

	uint64_t m_TimeDiff = 0;
	CRITICAL_SECTION m_CS;

	ChangeSettings m_Settings;

	bool m_Connected;
	bool m_Streaming;

	struct Request {
		uint64_t timestamp;
		sockaddr_in address;
		char deviceName[32];
	};
	std::list<Request> m_Requests;
};
