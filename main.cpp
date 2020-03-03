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

std::list<std::string> disconnected_clients;

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
        sub_msg msg;
        msg.topic_name = payload;
        msg.packet_id = 69;
        mqtt_subscribe(mqtt_socket, msg);
        try {
            subscriptions.at(std::string(payload)).push_back(sd);
        } catch (const std::out_of_range&) {
            subscriptions.insert(std::pair<std::string, std::list<int>>(std::string(payload), std::list<int>{sd}));
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
    int sd, activity, valread;
    int max_sd = 0;
    std::map<int, std::string>clients;
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 10000;

    mqtt_socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = { 0 }; // 1
    address.sin_family = AF_INET; // 3
    address.sin_port = htons(1883); // 4
    inet_pton(AF_INET, "192.168.1.48", &address.sin_addr);
    
    if (connect(mqtt_socket, (struct sockaddr *)&address, sizeof(address)) < 0) { // 6
        exit(1);
    }
    
    // connectToMqtt(mqtt_socket);

    int i = 0;
    for (auto section: ini.sections) {
        std::cout << section.first << std::endl;
        std::cout << section.second["mac"] << std::endl;

        std::string mac = section.second["mac"];
        // clients.insert(std::pair<int, std::string>(connectToSocket(mac.c_str()), mac));
        disconnected_clients.push_back(mac);
        i++;
    }

    while (true) {
        FD_ZERO(&readfds);        
        
        std::cout << "Set mqtt " << mqtt_socket << std::endl;

        for (auto it = disconnected_clients.begin(); it != disconnected_clients.end();) {
            std::string mac = *it;
            std::cout << "Trying to estabilish connection with " << mac << std::endl;
            int r_sd = connectToSocket(mac.c_str());
            if (r_sd < 0) {
                std::cout << "... Fail" << std::endl;
                ++it;
                continue;
            }
            std::cout << "**** SUCCESS ****" << std::endl;
            clients.insert(std::pair<int, std::string>(r_sd, mac));
            disconnected_clients.erase(it++);
        }

        for (auto it = clients.begin(); it != clients.end(); ++it) {
            sd = it->first;
            FD_SET(sd, &readfds);
            std::cout << "Set " << sd << std::endl;
            if (sd > max_sd) max_sd = sd;
        }

        FD_SET(mqtt_socket, &readfds);
        // max_sd = mqtt_socket;

        std::cout << "Max sd: " << max_sd << std::endl;
        activity = select( max_sd+1 , &readfds , NULL , NULL , NULL);
        std::cout << "Got activity" << std::endl;

        if ((activity < 0) && (errno!=EINTR)) {   
            std::cout << "select error " << errno << std::endl;   
            perror("Select error");
        }

        if (FD_ISSET(mqtt_socket, &readfds)) {
            std::cout << "#### ACTIVITY ON MASTER" << std::endl;
            char fixed_header[2] = { 0 };
            int n = recv(mqtt_socket, fixed_header, sizeof(fixed_header), 0);
            if (n < 0 || n == 0) {
                std::cout << "MQTT DISCONNECT: " << n << std::endl;
                reconnect:
                int connected = -1;
                do {
                    close(mqtt_socket);
                    mqtt_socket = socket(AF_INET, SOCK_STREAM, 0);
                    connected = connect(mqtt_socket, (struct sockaddr *)&address, sizeof(address));
                    if (connected != -1) {
                        connectToMqtt(mqtt_socket);
                    }
                } while (connected < 0);
                // continue;
                goto clients_flag;
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
        clients_flag:
        std::cout << "Going to clients " << clients.size() << std::endl;
        // readfds.fd_count = clients.size() + 1;
        // for (int i = 0; i < readfds.fd_count; )
        for (auto it = clients.begin(); it != clients.end();) {           
            sd = it->first;
            // std::cout << "Am i set? " << sd << std::endl;

            if (!FD_ISSET(sd, &readfds)) continue;
            std::cout << "******* SET ****** " << sd << std::endl;

            char buf[2];
            int recv_size;
            recv_size = recv(sd, &buf, sizeof(buf), MSG_WAITALL);
            bool erased = false;
            if (recv_size <= 0) {
disconnected:
                std::cout << "DISCONNECTED " << it->second << std::endl;
                disconnected_clients.push_back(it->second);
                it = clients.erase(it);
                erased = true;
                std::cout << "Erased" << std::endl;
                continue;
            }
            std::cout << "Test" << std::endl;
            if (!erased) {
                std::cout << "Not erased" << std::endl;
                ++it;
            }

            short x = short(buf[0] << 8 | buf[1]);

            char buffer[x+1];
            recv_size = recv(sd, &buffer, x, MSG_WAITALL);
            if (recv_size <= 0) goto disconnected;
            buffer[recv_size] = '\0';

            std::cout << "Got event" << std::endl;


            recv_size = recv(sd, &buf, sizeof(buf), MSG_WAITALL);
            if (recv_size <= 0) goto disconnected;
            x = short(buf[0] << 8 | buf[1]);

            char buffer2[x+1];
            recv_size = recv(sd, &buffer2, x, MSG_WAITALL);
            buffer2[recv_size] = '\0';
            callback(buffer, buffer2, sd);
        }
    }

	return 0;
}