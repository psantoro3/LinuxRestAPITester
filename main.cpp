
#include <csignal>
#include <thread>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <string.h>

using namespace std;

int main()
{

	struct sockaddr_in serverAddress;
	int tcpSocket = -1;
	char commandBuffer[50];
	char returnBuffer[500];
	double openTCPTimeoutMs = 1000.0;
	double commTCPTimeoutMs = 100.0;

	// look for the port number on the end of the IP address string. 
	// Change these values to test various requests
	std::string ipStr = "10.160.160.161";
	std::string portStr = "8008";
	std::string apiGET = "/api/v1/defrosting/0";

	// create the socket
	tcpSocket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (tcpSocket == -1) {
		cout << "Can't create socket- returning";		// test stuff
		return -1;
	}

	// assign the ip address and port
	bzero(&serverAddress, sizeof(serverAddress));
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = inet_addr(ipStr.c_str());
	serverAddress.sin_port = htons((unsigned short)stoi(portStr));

	// connect the socket 
	connect(tcpSocket, (sockaddr*)&serverAddress, sizeof(serverAddress));
	// as we're using non-blocking sockets, it will return an error on connect, of 'EININPROGRESS'.
	// this means that the port is connecting, but not done yet.  As this is expected, we will
	// proceed if that error is received, but no other.
	if (errno != EINPROGRESS) {
		// close up the socket, we didn't connect.
		close(tcpSocket);
		// save time of this attempt so we don't constantly try
		cout << "Unable to connect socket - returning";
		return -1;
	}

	// now wait for the port to open for writing
	struct timespec timeSpecNow;

	// pick up start time for timeout purposes
	clock_gettime(CLOCK_MONOTONIC, &timeSpecNow);
	double openTCPStartTimeMs = ((double)timeSpecNow.tv_sec * 1000.0) + ((double)timeSpecNow.tv_nsec / 1000000.0);
	double msNow = 0.0;

	int returnVal = 0;
	do {
		// set up for the 'select' command used to check the port opening
		fd_set fds{};
		FD_ZERO(&fds);
		FD_SET(tcpSocket, &fds);

		// We want the select to return quickly, so set the timeout to near zero
		timeval timeout{};
		timeout.tv_sec = 0;
		timeout.tv_usec = 5;

		// Use select() to check if the socket is connected and ready to write to.
		// A positive number return indicates success, a negative indicates error,
		// zero indicates it's not ready yet.
		returnVal = select(tcpSocket + 1, NULL, &fds, NULL, &timeout);

		clock_gettime(CLOCK_MONOTONIC, &timeSpecNow);
		msNow = ((double)timeSpecNow.tv_sec * 1000.0) + ((double)timeSpecNow.tv_nsec / 1000000.0);

	} while ((returnVal == 0) && (msNow < (openTCPStartTimeMs + openTCPTimeoutMs)));

	// check for a good open
	if (returnVal > 0) {

		// here we'll send an API call, and wait for a response
		int result;

		// Check that we have a valid socket
		if (tcpSocket == -1) {
			cout << "Bad socket number - returning";
			return -1;
		}

		// another type of check for connected?
		char buffer[32];
		if (recv(tcpSocket, buffer, sizeof(buffer), MSG_PEEK) == 0) {
			// if recv returns zero, that means the connection has been closed:
			// close up the socket
			close(tcpSocket);
			cout << "Socket can't receive - returning";		// test stuff
			return -1;
		}

		// set up for the 'select' command
		fd_set fds{};
		FD_ZERO(&fds);
		FD_SET(tcpSocket, &fds);

		// set up the timeout value
		timeval timeout{};
		timeout.tv_sec = (long int)(commTCPTimeoutMs / 1000.0);
		timeout.tv_usec = 0;

		// Is the socket ready to send?
		if (select(tcpSocket + 1, NULL, &fds, NULL, &timeout) == 0) {
			// we timed out
			// close up the socket
			close(tcpSocket);
			cout << "Timeout before send - returning";		// test stuff
			return -1;
		}

		cout << "Sending API Command";

		// Send the command
		sprintf(commandBuffer, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: Close\r\n\r\n", apiGET.c_str(), ipStr.c_str());
		try {
			result = send(tcpSocket, commandBuffer, strlen(commandBuffer), 0);
		}
		catch (...) {
			cout << "exception \n";
			result = -1;
		}
		if (result == -1) {
			// close up the socket
			close(tcpSocket);
			cout << "Exception while sending - returning";		// test stuff
			return -1;    // This probably means we're not connected.
		}

		// delay here - give the other side a bit to reply.
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		// Is the recv ready?
		if (select(tcpSocket + 1, &fds, NULL, NULL, &timeout) == 0) {
			// we timed-out
			// close up the socket
			close(tcpSocket);
			cout << "Timeout while waiting response - returning";		// test stuff
			return -1;
		}

		// The response is ready, receive it
		result = recv(tcpSocket, returnBuffer, 500, 0);
		string returnStr = returnBuffer;

		cout << "return:  " + returnStr;

		// close the connection
		// set up for the 'select' command
		FD_ZERO(&fds);
		FD_SET(tcpSocket, &fds);

		// We want the select to return quickly, so set the timeout to zero
		timeout.tv_sec = 0;
		timeout.tv_usec = 10;

		// If the socket is open, close it.
		if (tcpSocket != -1) {
			// disable any transmissions
			shutdown(tcpSocket, SHUT_WR);
			// Is there any data to receive before we close up?
			if (select(tcpSocket + 1, &fds, NULL, NULL, &timeout) != 0) {
				// receive it just to clear the buffer
				recv(tcpSocket, returnBuffer, 500, 0);
			}
			close(tcpSocket);
		}

		// first, find out if it's a good return.. should contain '200 OK'
		if (returnStr.find("200 OK") == std::string::npos) {
			cout << "Bad API string - returning";
			return -1;
		}

		size_t contentLength = 0;
		string contentStr;
		string payload;

		// check for 'chunked' transfer encoding
		if (returnStr.find("Transfer-Encoding: chunked") != std::string::npos) {

			cout << "Chunked Encoding found";

			// make a string of the content (everything past the blank line)
			contentStr = returnStr.substr(returnStr.find("\r\n\r\n") + 4);

			// get the first 'line' from the content (everything before the first cr/lf pair), 
			// then trim that line off the content
			string singleLine = contentStr.substr(0, contentStr.find("\r\n"));
			contentStr = contentStr.substr(contentStr.find("\r\n") + 2);

			// this line should be the length of the next 'chunk', in hex.  for this api call,
			// it should always be 19 decimal, 13 hex.
			contentLength = stoi(singleLine, nullptr, 16);
	
			// get the next 'line', which will be the date/time from the API.  It should be
			// exactly the length given in the first chunk size.
			singleLine = contentStr.substr(0, contentStr.find("\r\n"));

			// transfer it to another string for sending in a command
			payload = singleLine;
		}
		else {
			// if we're not chunked, we'll look for content-length, and just use the content
			// directly.
			unsigned int pos = returnStr.find("Content-Length: ");
			if (pos == std::string::npos) {
				cout << "No content-length string - returning";		// test stuff
				return -1;
			}

			// get the content length
			int lenLen = returnStr.find("\r\n", pos) - pos - 16;	// 16 is the length of 'Content-Length: '
			string lenStr = returnStr.substr(pos + 16, lenLen);
			contentLength = stoi(lenStr);
			if (contentLength != 19) {
				return -1;
			}

			// make a string of the content (everything past the blank line, for the length given)
			contentStr = returnStr.substr(returnStr.find("\r\n\r\n") + 4, contentLength);

			// transfer it to another string for sending in a command
			payload = contentStr;
		}

		cout << payload;
		return 0;

	}
}