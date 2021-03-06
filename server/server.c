#include "server.h"

/* Static functions */

static char * cmap (char * map, uint32_t x, uint32_t y, uint32_t width) {
	return &map[x + y * width];
}

/* Recv/send with endianness conversion */
static int recvMessages (int sock, wireworld_message_t * buffer, uint32_t count);
static int sendMessages (int sock, wireworld_message_t * buffer, uint32_t count);

/* Network to char map conversion */
static void networkToCharMap (wireworld_message_t * networkMap, char * charMap,
		uint32_t width, uint32_t height);
void charToNetworkMap (wireworld_message_t * networkMap, char * charMap,
		uint32_t width, uint32_t height,
		uint32_t xs, uint32_t ys, uint32_t xe, uint32_t ye);

/* Server functions */

int serverInit (int port) {
	int sock;

	sock = socket (AF_INET6, SOCK_STREAM, 0);
	if (sock != -1) {
		struct sockaddr_in6 saddr;
		
		memset (&saddr, 0, sizeof (saddr));
		saddr.sin6_family = AF_INET6;
		saddr.sin6_port = htons (port);
		saddr.sin6_addr = in6addr_any;

		int res = bind (sock, (const struct sockaddr*) &saddr, sizeof (saddr));
		if (res != -1) {
			if (listen (sock, SERVER_BACKLOG) != -1) {
				return sock;
			} else {
				perror ("listen");
				close (sock);
			}
		} else {
			perror ("bind");
			close (sock);
		}
	} else {
		perror ("socket");
	}
	return -1;
}

int serverAccept (int serverSock) {
	assert (serverSock != -1);
	int sock = accept (serverSock, NULL, NULL);
	if (sock == -1)
		perror ("accept");
	return sock;
}

/* Connection functions */
int connectionWaitForInit (int connSock,
		uint32_t * width, uint32_t * height, uint32_t * sampling, char ** firstFrame) {
	wireworld_message_t message[4];

	assert (width != NULL);
	assert (height != NULL);
	assert (sampling != NULL);
	assert (firstFrame != NULL);

	if (recvMessages (connSock, message, 4) == 0 && message[0] == R_INIT) {
		// If init message, retrieve sizes and sampling
		*width = message[1];
		*height = message[2];
		*sampling = message[3];

		// Prepare buffer to read init map
		uint32_t data_size = wireworldFrameMessageSize (*width, *height);
		wireworld_message_t * buf = malloc (data_size * sizeof (wireworld_message_t));
		assert (buf != NULL);

		// Read map
		if (recvMessages (connSock, buf, data_size) == 0) {
			// Alloc char map
			*firstFrame = malloc (*width * *height * sizeof (char));
			assert (*firstFrame != NULL);

			// Convert network map to char map
			networkToCharMap (buf, *firstFrame, *width, *height);

			// destroy temp buffer
			free (buf);
			return 0;
		} else {
			fprintf (stderr, "Unable to read the first frame\n");
			free (buf);
			return -1;
		}
	} else {
		fprintf (stderr, "Received something which is not an init message\n");
		return -1;
	}
}

int connectionSendFullUpdate (int connSock,
		char * charMap, uint32_t width, uint32_t height,
		uint32_t localXStart, uint32_t localYStart, uint32_t localXEnd, uint32_t localYEnd) {
	// Send a complete rect update followed by an end of frame
	int res = connectionSendRectUpdate (connSock,
				charMap, width, height,
				localXStart, localYStart, localXEnd, localYEnd,
				0, 0);
	int res2 = connectionSendFrameEnd (connSock);
	
	// Checking time
	if (res == 0 && res2 == 0) {
		return 0; // All went well
	} else if (res == 1 || res2 == 1) {
		return 1; // End of connection
	} else {
		fprintf (stderr, "Error while sending full update\n");
		return -1;
	}
}

int connectionWaitFrameRequest (int connSock) {
	assert (connSock != -1);

	wireworld_message_t message;
	int res = recvMessages (connSock, &message, 1);
	if (res == 0) {
		if (message == R_FRAME) {
			return 0;
		} else {
			fprintf (stderr, "Expected R_FRAME message but got something else : %u\n", message);
		}
	} else if (res == 1) {
		return 1;
	} else {
		fprintf (stderr, "Error while receiving R_FRAME request\n");
	}
	return -1;
}

int connectionSendRectUpdate (int connSock,
		char * charMap, uint32_t width, uint32_t height,
		uint32_t localXStart, uint32_t localYStart, uint32_t localXEnd, uint32_t localYEnd,
		uint32_t realXStart, uint32_t realYStart) {
	int ret = -1;
	assert (connSock != -1);
	assert (charMap != NULL && width > 0 && height > 0);
	assert (0 < localXStart && localXStart < localXEnd && localXEnd < width);
	assert (0 < localYStart && localYStart < localYEnd && localYEnd < height);

	// Init header message
	wireworld_message_t message[5];
	message[0] = A_RECT_UPDATE;
	message[1] = realXStart;
	message[2] = realYStart;
	message[3] = realXStart + localXEnd - localXStart;
	message[4] = realYStart + localYEnd - localYStart;
	
	// Send it
	int res = sendMessages (connSock, message, 5);
	if (res == 0) {
		// Prepare buffer
		uint32_t data_size = wireworldFrameMessageSize (
				localXEnd - localXStart,
				localYEnd - localYStart);
		wireworld_message_t * buf = malloc (data_size * sizeof (wireworld_message_t));
		assert (buf != NULL);

		// Convert
		charToNetworkMap (buf,
				charMap, width, height,
				localXStart, localYStart, localXEnd, localYEnd);

		// Send data
		int res2 = sendMessages (connSock, buf, data_size);
		if (res2 == 0) {
			ret = 0;
		} else if (res2 == 1) {
			ret = 1;
		} else {
			fprintf (stderr, "Error while sending A_RECT_UPDATE data\n");
		}

		free (buf);
	} else if (res == 1) {
		ret = 1;
	} else {
		fprintf (stderr, "Error while sending A_RECT_UPDATE header\n");
	}
	return ret;
}

int connectionSendFrameEnd (int connSock) {
	assert (connSock != -1);
	wireworld_message_t message = A_FRAME_END;
	int res = sendMessages (connSock, &message, 1);
	if (res == -1) {
		fprintf (stderr, "Error while sending A_FRAME_END\n");
		return -1;
	} else if (res == 1) {
		return 1; // End of connection
	} else {
		return 0;
	}
}

/* Static functions */

static int recvMessages (int sock, wireworld_message_t * buffer, uint32_t count) {
	uint32_t bytes_to_read = count * sizeof (wireworld_message_t);
	wireworld_message_t * tmp_buf = malloc (bytes_to_read);
	assert (tmp_buf != NULL);

	// Read raw data
	char * it = (char *) tmp_buf;
	while (bytes_to_read > 0) {
		int res = read (sock, it, bytes_to_read);
		if (res == 0) {
			free (tmp_buf);
			return 1; // End of file
		} else if (res == -1) {
			perror ("read");
			free (tmp_buf);
			return -1;
		}	
		it += res;
		bytes_to_read -= res;
	}

	// Convert endianness
	uint32_t i;
	for (i = 0; i < count; ++i)
		buffer[i] = ntohl (tmp_buf[i]);

	free (tmp_buf);

	return 0;
}

static int sendMessages (int sock, wireworld_message_t * buffer, uint32_t count) {
	uint32_t bytes_to_send = count * sizeof (wireworld_message_t);
	wireworld_message_t * tmp_buf = malloc (bytes_to_send);
	assert (tmp_buf != NULL);

	// Convert endianness
	uint32_t i;
	for (i = 0; i < count; ++i)
		tmp_buf[i] = htonl (buffer[i]);

	// Send raw data
	char * it = (char *) tmp_buf;
	while (bytes_to_send > 0) {
		int res = send (sock, it, bytes_to_send, MSG_NOSIGNAL);
		if (res == -1) {
			if (errno == EPIPE || errno == ECONNRESET) {
				// On end of connection
				return 1;
			} else {
				// Real error
				perror ("write");
				free (tmp_buf);
				return -1;
			}
		}	
		it += res;
		bytes_to_send -= res;
	}
	free (tmp_buf);
	return 0;
}

static void networkToCharMap (wireworld_message_t * networkMap, char * charMap,
		uint32_t width, uint32_t height) {
	// Bit packed structure iterators
	uint32_t messageIndex = 0;
	int bitIndex = 0;

	// Iterate on coordinates and unpack data
	uint32_t i, j;
	for (j = 0; j < height; ++j) {
		for (i = 0; i < width; ++i) {
			// Set value
			*cmap (charMap, i, j, width) = C_BIT_MASK & 
				(networkMap[messageIndex] >> (C_BIT_SIZE * bitIndex));

			// Update counters
			bitIndex++;
			if (bitIndex == M_BIT_SIZE / C_BIT_SIZE) {
				messageIndex++;
				bitIndex = 0;
			}
		}
	}
}

void charToNetworkMap (wireworld_message_t * networkMap, char * charMap,
		uint32_t width, uint32_t height,
		uint32_t xs, uint32_t ys, uint32_t xe, uint32_t ye) {
	(void) height;

	// iterators
	uint32_t messageIndex = 0;
	int bitIndex = 0;
	uint32_t i, j;

	networkMap[0] = 0;
	for (j = ys; j < ye; ++j) {
		for (i = xs; i < xe ; ++i) {
			// Set value
			networkMap[messageIndex] |=
				*cmap (charMap, i, j, width) <<
				(C_BIT_SIZE * bitIndex);

			// Update counters, and init next word
			bitIndex++;
			if (bitIndex == M_BIT_SIZE / C_BIT_SIZE) {
				messageIndex++;
				networkMap[messageIndex] = 0;
				bitIndex = 0;
			}
		}
	}
}

