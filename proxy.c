// Jonathan Stock
// COEN 162 Code Project 1 Web Proxy
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

int main()
{
   int welcomeSocket, newSocket;
   char buffer[1024];
   char buffercopy[1024];
   struct sockaddr_in serverAddr;
   struct sockaddr_storage serverStorage;
   socklen_t addr_size;
   // CREATE SERVER SOCKET
   /*---- Create the socket. The three arguments are: ----*/
   /* 1) Internet domain 2) Stream socket 3) Default protocol (TCP in this case) */
   welcomeSocket = socket(PF_INET, SOCK_STREAM, 0);
   /*---- Configure settings of the server address struct ----*/
   /* Address family = Internet */
   serverAddr.sin_family = AF_INET;
   /* Set port number, using htons function to use proper byte order */
   serverAddr.sin_port = htons(8080);
   /* Set IP address to localhost */
   unsigned long address = INADDR_NONE;
   address = inet_addr("127.0.0.1");
   serverAddr.sin_addr.s_addr = address;
   /* Set all bits of the padding field to 0 */
   memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

   /*---- Bind the address struct to the socket ----*/
   bind(welcomeSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
   /*---- Listen on the socket, with 5 max connection requests queued ----*/
   if (listen(welcomeSocket, 5) == 0)
      printf("Listening\n");

   else
      printf("Error\n");

   while (1)
   {
      // BROWSER INFO IS IN PROXY
      // newSocket is PROXY
      /*---- Accept call creates a new socket for the incoming connection ----*/
      addr_size = sizeof(serverStorage);
      newSocket = accept(welcomeSocket, (struct sockaddr *)&serverStorage, &addr_size);

      /*---- Send message to the socket of the incoming connection ----*/
      read(newSocket, buffer, 1023);

      strcpy(buffercopy, buffer);
      int i = 0;
      char *token = strtok(buffercopy, "\r\n");
      while (i != 1)
      {
         token = strtok(NULL, "\r\n");
         i += 1;
      }
      // PRINTS Host: www.example.com
      printf("%s\n", token);

      char *name = NULL;
      name = strtok(token, " "); // find the first space
      name = strtok(NULL, " ");  // find the second space

      // PRINTS www.example.com
      printf("%s\n", name);
      int size = strlen(name);
      // PRINTS SIZE: 15 (length of the url)
      printf("SIZE: %d\n", size);

      // translate hostname to ip address
      struct hostent *host;
      struct in_addr ipaddy;
      host = gethostbyname(name);
      ipaddy = *(struct in_addr *)(host->h_addr);

      // CREATE CLIENT SOCKET
      int clientSocket;
      struct sockaddr_in clientAddr;
      char buffer2[1024];
      socklen_t addr_size2;
      addr_size2 = sizeof(clientAddr);

      /*---- Create the socket. The three arguments are: ----*/
      /* 1) Internet domain 2) Stream socket 3) Default protocol (TCP in this case) */
      if ((clientSocket = socket(PF_INET, SOCK_STREAM, 0)) < 0)
      {
         printf("Failed to create socket for %s.\n", name);
         exit(1);
      }

      else
      {
         printf("Socket created for %s.\n", name);
      }

      /*---- Configure settings of the server address struct ----*/
      /* Address family = Internet */
      clientAddr.sin_family = AF_INET;

      /* Set port number, using htons function to use proper byte order */
      clientAddr.sin_port = htons(80);

      /* Set IP address to localhost */
      unsigned long address2 = INADDR_NONE;
      address2 = inet_addr(inet_ntoa(ipaddy));

      clientAddr.sin_addr.s_addr = address2;

      printf("Printing IP Address: %s\n", inet_ntoa(ipaddy));

      /* Set all bits of the padding field to 0 */
      memset(clientAddr.sin_zero, '\0', sizeof clientAddr.sin_zero);

      /*---- Connect the socket to the server using the address struct ----*/
      connect(clientSocket, (struct sockaddr *)&clientAddr, addr_size2);
      printf("Connected client\n");

      /*---- send the message from the buffer into the client socket ----*/
      send(clientSocket, buffer, strlen(buffer), 0);
      printf("Sent message from client\n");

      printf("Before while loop\n");
      printf("%s\n", buffer2);

      int n;
      while ((n = read(clientSocket, buffer2, 1024)) > 0)
      {
         printf("In while loop\n");

         write(newSocket, buffer2, n);
         printf("%s\n", buffer2);
         bzero(buffer2, n);
      }
      printf("After while loop\n");

      close(newSocket);
      close(clientSocket);
   }
   return 0;
}