#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <fstream>
#include <sys/select.h>
#include<ctime>
#include <sys/ioctl.h>
#include <chrono>
#include <arpa/inet.h>
#include <list>

#include "lib/inipp.h"
#include "lib/mqtt.cpp"

using namespace std::chrono;

int mqtt_socket;

std::map<std::string, std::list<int>> subscriptions;

std::map<int, std::string> disconnected_clients;

void sendToBT(int bd, char *event, char *payload) {
    char buffer[strlen(event)+strlen(payload)+4];
    int size = addString(-1, buffer, event);
    size = addString(size, buffer, payload);
    int wb = write(bd, buffer, sizeof(buffer));
    std::cout << ">> " << event << std::endl;
    // std::cout << wb << std::endl;
}

int connectToSocket(const char *address) {
    struct sockaddr_rc addr = { 0 };
    int s, status;

    char dest[250];
    strcpy(dest, address);

    s = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);

    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = (uint8_t) 1;
    str2ba( dest, &addr.rc_bdaddr );

    status = connect(s, (struct sockaddr *)&addr, sizeof(addr));

    if( status < 0 ) {
        return -1;
    };

    sendToBT(s, "PING", "");
    sendToBT(s, "MQTT_INIT", "");

    return s;
}


void connectToMqtt(int fd) {
    conn_message c_msg;
    // c_msg.flags = CLEAN_START;
    c_msg.flags = 0;
    c_msg.keep_alive = 2;
    c_msg.client_id = "BluetoothGate";

    mqtt_connect(fd, c_msg);
}

void callback(char *topic_cstr, char *payload, int sd) {
    std::string topic = std::string(topic_cstr);
    std::cout << "<< " << topic << " - " << payload << std::endl;

    if (topic == "MQTT_SUBSCRIBE") {
        std::cout << "SUBBING" << std::endl;
        sub_msg msg;
        msg.topic_name = payload;
        msg.packet_id = 69;
        mqtt_subscribe(mqtt_socket, msg);
        try {
            subscriptions.at(std::string(payload)).push_back(sd);
            std::cout << "ADD" << std::endl;
        } catch (const std::out_of_range&) {
            subscriptions.insert(std::pair<std::string, std::list<int>>(std::string(payload), std::list<int>{sd}));
            std::cout << "INSERT" << std::endl;
        }
    }
}


int main(int argc, char **argv)
{
    inipp::Ini<char> ini;
	std::ifstream is("config/devices.ini");
	ini.parse(is);

    fd_set readfds;
    int max_clients = 30;
    int sd, max_sd, activity, valread;
    std::map<int, std::string>clients;
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000;

    mqtt_socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = { 0 }; // 1
    address.sin_family = AF_INET; // 3
    address.sin_port = htons(1883); // 4
    inet_pton(AF_INET, "192.168.1.48", &address.sin_addr);
    
    if (connect(mqtt_socket, (struct sockaddr *)&address, sizeof(address)) < 0) { // 6
        exit(1);
    }
    
    connectToMqtt(mqtt_socket);

    int i = 0;
    for (auto section: ini.sections) {
        std::cout << section.first << std::endl;
        std::cout << section.second["mac"] << std::endl;

        std::string mac = section.second["mac"];
        // sd = connectToSocket(mac.c_str());
        disconnected_clients.insert(std::pair<int, std::string>(-1, mac));
        std::cout << "Inserted  " << sd << std::endl;
        i++;
    }

    while (true) {
        FD_ZERO(&readfds);
        for (auto it = disconnected_clients.begin(); it != disconnected_clients.end();) {
            sd = it->first;
            std::string mac = it->second;
            
            for (auto c_it = clients.begin(); c_it != clients.end();) {
                if (c_it->second == mac) {
                    clients.erase(c_it++);
                    continue;
                }
                ++c_it;
            }

            int new_sd = connectToSocket(mac.c_str());
            if (new_sd < 0) continue;

            clients.insert(std::pair<int, std::string>(new_sd, mac));
            it = disconnected_clients.erase(it);
            std::cout << "RECONNECTED. Clients: " << clients.size() << ", Disconnected clients: " << disconnected_clients.size() << std::endl;
        }

        for (auto it = clients.begin(); it != clients.end(); ++it) {
            sd = it->first;
            FD_SET(sd, &readfds);
            auto search = disconnected_clients.find(sd);
            if (search != disconnected_clients.end()) {
                disconnected_clients.erase(sd);
                std::cout << "Erased" << std::endl;
            }

            if (sd > max_sd) max_sd = sd;
        }

        FD_SET(mqtt_socket, &readfds);

        activity = select( max_sd + 1 , &readfds , NULL , NULL , NULL);

        if ((activity < 0) && (errno!=EINTR)) {   
            std::cout << "select error" << std::endl;   
        }

        if (FD_ISSET(mqtt_socket, &readfds)) {
            char fixed_header[2] = { 0 };
            int n = recv(mqtt_socket, fixed_header, sizeof(fixed_header), 0);
            if (n < 0 || n == 0) {
    reconnect:
                int connected = -1;
                do {
                    close(mqtt_socket);
                    mqtt_socket = socket(AF_INET, SOCK_STREAM, 0);
                    connected = connect(mqtt_socket, (struct sockaddr *)&address, sizeof(address));
                } while (connected < 0);

                connectToMqtt(mqtt_socket);
                continue;
            }

            char msg[fixed_header[1]];
            n = recv(mqtt_socket, msg, sizeof(msg), 0);
            if (n < 0 || n == 0) {
                goto reconnect;
            }

            if (fixed_header[0] == CONN_ACK) {

            } else if (fixed_header[0] == SUB_ACK) {
                std::cout << "SUBBED" << std::endl;
            } else if (fixed_header[0] == PUB) {
                short topic_len = short(msg[0] << 8 | msg[1]);
                char topic[topic_len+1] = {0};
                
                int payload_len = fixed_header[1]-2-topic_len+1;
                char payload[payload_len+1] = {0};
                
                substr(msg, 2, 2+topic_len, topic);
                substr(msg, 2+topic_len, fixed_header[1], payload);

                bool found;
                try {
                    subscriptions.at(std::string(topic));
                    found = true;
                    std::cout << "TOPIC FOUND" << std::endl;    
                    for (int r_sd: subscriptions.at(std::string(topic))) {
                        sendToBT(r_sd, topic, payload);
                        std::cout << "SENT TO " << r_sd << std::endl;
                    }
                } catch (const std::out_of_range&) {
                    std::cout << "out of range" << std::endl;
                }
                
            }
        }

        for (auto it = clients.begin(); it != clients.end(); ++it) {
            sd = it->first;
            if (!FD_ISSET(sd, &readfds)) continue;

            char buf[2];
            int recv_size;
            recv_size = recv(sd, &buf, sizeof(buf), MSG_WAITALL);

            if (recv_size <= 0) {
                std::cout << "DISCONNECTED " << it->second << std::endl;
                disconnected_clients.insert(std::pair<int, std::string>(it->first, it->second));
                for (auto s_it = subscriptions.begin(); s_it != subscriptions.end(); ++s_it) {
                    for (auto sd_it = s_it->second.begin(); sd_it != s_it->second.end();) {
                        int descriptor = *sd_it;
                        if (descriptor == sd) {
                            sd_it = s_it->second.erase(sd_it);
                            std::cout << "Erased SD from subscription " << s_it->first << std::endl;
                        } else {
                            ++sd_it;
                        }
                    }
                }
                continue;
            }

            short x = short(buf[0] << 8 | buf[1]);

            char buffer[x+1];
            recv_size = recv(sd, &buffer, x, MSG_WAITALL);
            buffer[recv_size] = '\0';

            recv_size = recv(sd, &buf, sizeof(buf), MSG_WAITALL);
            x = short(buf[0] << 8 | buf[1]);

            char buffer2[x+1];
            recv_size = recv(sd, &buffer2, x, MSG_WAITALL);
            buffer2[recv_size] = '\0';
            callback(buffer, buffer2, sd);
        }
    }

	return 0;
}