#undef UNICODE

#define WIN32_LEAN_AND_MEAN             // vylou�en� n�kter�ch hlavi�kov�ch soubor� p�i windows.h
#define _CRT_SECURE_NO_WARNINGS         // ignorov�n� chyb, zejm�na pro mo�nost pou��t scanf

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <stdio.h>
#include <thread>
#include <string>
#include <sstream>

#pragma comment (lib, "Ws2_32.lib")

#define DEFAULT_BUFLEN 512    // buffer nastaven na 512 byt�
#define DEFAULT_PORT "3999"   // port pro testov�n� aplikace

using namespace std;

class TimeOutException {};  // v�jimka pro timeout
class SyntaxError {};       // chyba syntaxe
class LogicError {};        // logick� chyba
class OtherError {          // nedefinovan� dal�� chyba, pro konstukci p�ed�v� sting s popisem chyby
    string message;
public:
    OtherError(string message)
        : message(message)
    {}
};
class ClientDisconnectedException {};       // v�jimka p�i odpojen� robota

void myAssert(bool cond, string message)    // pouze dopl�kov� funkce pro ov��en� n�vratov� hodnoty a vyho�en� chyby  
{
    if (!cond)
        throw OtherError(message);
}

string dropLastChars(string input, int lastCount)     // pou��v�no pouze pro odd�l�n� \a\b z �et�zc�
{
    return input.substr(0, input.length() - lastCount);
}

class Connection // t��da connection je pou��v�na jako rozhran� pro odes�l�n�, p�ij�m�n� a �etn� zpr�v
{
private:
    SOCKET clientSocket;
    char recvbuf[DEFAULT_BUFLEN];
    int recvbuflen = DEFAULT_BUFLEN;
    int received = 0;
    int buffPosition = 0;
    char ReadChar()             // funkce pro �ten� zpr�v, �te se po charakterech zvl�t
    {
        if (buffPosition == received)
        {
            received = recv(clientSocket, recvbuf, recvbuflen, 0);  // recv vrac� kladn� po�et pro �sp�sne p�ijet� x bytu, 0 pokud bylo spojeni validne ukonceno, pro dalsi chyby jinak
            if (received > 0) {
                printf("Bytes received: %d\n", received);
            }
            else if (received == 0)
                throw ClientDisconnectedException();
            else {
                int errorNumber = WSAGetLastError();
                if (errorNumber == WSAETIMEDOUT)
                {
                    throw TimeOutException();
                }
                else {
                    throw OtherError("recv failed with error: " + errorNumber);
                }
            }
            buffPosition = 0;
        }
        return recvbuf[buffPosition++];
    }
    Connection(const Connection&) = delete;         // t��du connection nelze kop�rovat
public:
    Connection(SOCKET clientSocket)                 // konstruktor connection, p�ed�v� socket klienta
        : clientSocket(clientSocket)
    {}
    void SetTimeOut(int seconds)                    // nastaven� timeoutu pro toto spojen�
    {
        // https://stackoverflow.com/questions/1824465/set-timeout-for-winsock-recvfrom
        // https://docs.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-setsockopt
        int iTimeout = seconds * 1000;
        int iRet = setsockopt(clientSocket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            (const char*)&iTimeout,
            sizeof(iTimeout)
        );
        printf("Timeout set. Return value: %d\n", iRet);
    }
    string ReadLine(int max)    // �ten� zpr�vy a jej� navr�cen�
    {
        if (max < 12) max = 12;
        std::stringstream ss;
        char c = 0;
        int count = 0;
        char prev = 0;
        while (true)
        {
            prev = c;
            c = ReadChar();
            ss << c;
            count++;
            if ((prev == '\a') && (c == '\b'))
            {
                return dropLastChars(ss.str(), 2);
            }
            if (count == max)
                throw SyntaxError();
        }
        return ss.str();
    }
    void WriteLine(string message)  // odesl�n� zpr�vy, zpr�va se p�ed�v� jako parametr a p�id�vaj� se ukon�ovac� znaky
    {
        int iSendResult = send(clientSocket, message.c_str(), message.length(), 0);
        myAssert(iSendResult != SOCKET_ERROR, "send failed with error: " + WSAGetLastError());
        printf("Bytes sent: %d\n", iSendResult);
        iSendResult = send(clientSocket, "\a\b", 2, 0);
        myAssert(iSendResult != SOCKET_ERROR, "send failed with error: " + WSAGetLastError());
        printf("Bytes sent: %d\n", iSendResult);
    }
};

int positivePart(int x) // pou��v�no pro pohyby robota, vrac� parametr, pokud je kladn�, jinak 0
{
    return max(0, x);
}

int maxIndex(int x0, int x1)    // vrac� 0, pokud je x0 v�t��, jinak 1
{
    if (x0 > x1)
        return 0;
    else
        return 1;
}

int maxIndex(int x0, int x1, int x2, int x3)    // tato funkce se pou��v� pro zji�t�n� sm�ru, kter�m je t�eba se pohybovat k c�li
{
    int x01 = max(x0, x1);
    int i01 = maxIndex(x0, x1);
    int x23 = max(x2, x3);
    int i23 = maxIndex(x2, x3);
    if (x01 > x23)
        return i01;
    else
        return i23 + 2;
}

class Robot         // t��da robot reprezentuje ji� rozhran� samotn�ch 
{
private:
    Connection* connection;     // p�ipojen� robota pro komunikaci se serverem
    int direction;              // jak�m sm�rem je robot oto�en
    int x;
    int y;
public:
    Robot(Connection* connection)   // konstruktor pro robota, p�ed�v�me mu connection jako parametr
        :connection(connection)
    {
        connection->SetTimeOut(1);  // defaultn� timeout, ne p�i rechargingu, kdy� neobr��me zpr�vu od robota
    }
private:
    uint16_t hashString(string input)   // hashovac� funkce pro string, pou��v�no pro u�ivatelk� jm�no, p�et�k�n� je za��zeno datov�m typem
    {
        unsigned int sum = 0;
        for (int i = 0; i < input.length(); i++)
        {
            sum += (unsigned char)input[i];
        }
        sum *= 1000;
        return sum;
    }
    string readLineAfterRecharging(int max)     // p�es tuhle funkci nejprve �tu zpr�vy, p�edpokl�d�m, �e m��e doj�t k dob�jen�, proto�e p�i n�m mus�m m�nit timeout
    {
        string line = connection->ReadLine(max);    // �tu ��dku
        while (line == "RECHARGING")                // pokud se nabij�, p�enastav timeout, �ekej na FULL POWER, pokud dodjde jin� ne� FULL POWER, nast�v� logick� chyba, jinak p�enastav timeout zp�t, pokud se nabij� d�l, opakuj, jinak vra� dal�� na�tenou zpr�vu
        {
            connection->SetTimeOut(5);
            line = connection->ReadLine(12);
            if (line != "FULL POWER")
                throw LogicError();
            connection->SetTimeOut(1);
            line = connection->ReadLine(max);
        }
        return line;
    }

    struct Keys {               // struktura autentiza�n�ch kl��� pro p�r serveru a klienta
        uint16_t serverKey;
        uint16_t clientKey;
    };

    uint16_t parseNumber(string str)    // p�evod stringu na ��slo
    {
        try {
            int n = std::stoi(str);
            uint16_t res = n;
            if (to_string(res) != str)
                throw SyntaxError();
            return res;
        }
        catch (...) {
            throw SyntaxError();
        }
    }

    void authentication()       // autentiza�n� funkce pro p�ihl�en� robota pro komunikaci
    {
        Keys keys[5] = {        // p�ry autentiza�n�ch kl���
            {23019, 32037},
            {32037, 29295},
            {18789, 13603},
            {16443, 29533},
            {18189, 21952}
        };
        string username = readLineAfterRecharging(20);      // p�e�ti u�vatelsk� jm�no robota
        connection->WriteLine("107 KEY REQUEST");           // po�lu ��dost o kl��
        uint16_t keyID = parseNumber(readLineAfterRecharging(5));   // p�e�ti kl�� a p�eve� ho na uint16_t
        if (keyID < 0 || keyID >= 5)                            // ov��en�, jestli je kl�� v rozsahu
        {
            connection->WriteLine("303 KEY OUT OF RANGE");
            throw OtherError("key range");
        }
        Keys keyPair = keys[keyID];         // p�irazen� p�ru kl��i
        uint16_t hash = hashString(username);   // zahashuj uzivatelske jmeno
        uint16_t confirmation1 = hash + keyPair.serverKey;  // potvrzen� serveru
        uint16_t confirmation2expected = hash + keyPair.clientKey;  // o�ek�van� potvrzen� od robota 
        connection->WriteLine(to_string(confirmation1));        // ode�li potvrzen� robotovi
        uint16_t confirmation2 = parseNumber(readLineAfterRecharging(7));   // p�e�ti potvrzen� od robota
        if (confirmation2 != confirmation2expected)     // porovnej jestli sed� 
        {
            connection->WriteLine("300 LOGIN FAILED");
            throw OtherError("");
        }
        connection->WriteLine("200 OK");        // �sp�n� p�ipojen� 
    }
    void setDirection(int dx, int dy)       // nastaven� sm�ru robota
    {
        if (dy == 1)            
            direction = 0;      // up
        if (dx == 1)
            direction = 1;      // right
        if (dy == -1)
            direction = 2;      // down
        if (dx == -1)
            direction = 3;      // left
    }
    void readCoordinates()      // p�e�ten� sou�adnic robota po OK zpr�v�
    {
        string line = readLineAfterRecharging(12);
        int n;
        int res = sscanf(line.c_str(), "OK %d %d%n", &x, &y, &n);
        if (res != 2 || n != line.length())
            throw SyntaxError();
    }
    bool move()     // vyvol�n� pohybu u robota
    {
        connection->WriteLine("102 MOVE");  // po�li zpr�vu MOVE
        int oldX = x;
        int oldY = y;
        readCoordinates();                  // p�e�ti nov� sou�adnice robota
        if (oldX != x || oldY != y)         // pohnul se?
        {
            setDirection(x - oldX, y - oldY);   // aktualizuj sm�r
            return true;
        }
        return false;                       // nepohnul se
    }
    void turnLeft()                 // oto� se do leva
    {
        connection->WriteLine("103 TURN LEFT");
        readCoordinates();          // po oto�en� rovn� �ek�me OK
    }
    void turnRight()                // oto� se do prava
    {
        connection->WriteLine("104 TURN RIGHT");
        readCoordinates();          // po oto�en� rovn� �ek�me OK
    }
    void turn(int direction)    // zm�na sm�ru
    {
        int directionChange = (direction - this->direction + 4) % 4;
        if (directionChange == 1)   // right
            turnRight();
        if (directionChange == 2)   // up/down
        {
            turnRight();
            turnRight();
        }
        if (directionChange == 3)   // left
            turnLeft();
    }
    void goAndAvoidObstacle()       // jdi, pokud ses nepohnul, jdi do leva, pokud zase, jdi do prava
    {
        if (!move())
        {
            turnLeft();
            move();
            turnRight();
            move();
        }
    }
    void goTo(int direction)        // oto� se sm�rem direction, jdi, pokud naraz� na p�ek�ku, p�ekonej...
    {
        turn(direction);
        goAndAvoidObstacle();
    }
    int mostNeededDirection()       // funkce pro zji�t�n� sm�ru, kter�m jsme nejd�le k c�li, tedy p�jdeme j�m
    {
        return maxIndex(
            positivePart(-y),
            positivePart(-x),
            positivePart(y),
            positivePart(x)
        );
    }
    void goToTarget()       // vypo�ti nejv�ce pot�ebn� sm�r, oto� se j�m, jdi, pokud naraz� na p�ek�ku, oto� se...
    {
        goTo(mostNeededDirection());
    }
    void goToCenterOfCoordinateSystem()     // volej chozen�, dokud nejsme ve st�edu
    {
        do {
            goToTarget();
        } while (x != 0 || y != 0);
    }
    void getMessage()       // dej p��kaz k vyzvednut� zpr�vy, p�e�ti ji a odhla� robota
    {
        connection->WriteLine("105 GET MESSAGE");
        readLineAfterRecharging(100);
        connection->WriteLine("106 LOGOUT");
    }
public:
    void readLoop()     // ��d�c� funkce pro f�ze robota, nejd��ve prove� autentizaci, pot� dojdi do st�edu, vyzvedni zpr�vu
    {
        authentication();
        goToCenterOfCoordinateSystem();
        getMessage();
    }
};

int clientThread(SOCKET ClientSocket)       // funkce pro inicializaci p�ipojen�, p�i�azen� robotovi, d�le se vol� readLoop, kter� ��d� robota a sna��me se odchytit v�jimky 
{
    Connection connection(ClientSocket);
    try
    {
        Robot r(&connection);
        r.readLoop();
    }
    catch (SyntaxError) {
        connection.WriteLine("301 SYNTAX ERROR");
    }
    catch (LogicError) {
        connection.WriteLine("302 LOGIC ERROR");
    }
    catch (TimeOutException) {
        printf("Timeout.\n");
    }
    catch (ClientDisconnectedException)
    {
        printf("Client disconnected.\n");
    }
    catch (OtherError)
    {
        printf("Other error.\n");
    }
    printf("Connection closing...\n");              // ��dn� ukon�en� spojen� 
    int iResult = shutdown(ClientSocket, SD_SEND);
    if (iResult == SOCKET_ERROR) {
        printf("shutdown failed with error: %d\n", WSAGetLastError());
        closesocket(ClientSocket);
        return 1;
    }

    closesocket(ClientSocket);          // uzav�en� socketu

    printf("Thread ended...\n");        // vl�kno kon��
}

int __cdecl main(void)
{
    // https://docs.microsoft.com/en-us/windows/win32/winsock/complete-server-code
    WSADATA wsaData;
    int iResult;

    SOCKET ListenSocket = INVALID_SOCKET;   // nastaven� winsock socketu

    struct addrinfo* result = NULL;
    struct addrinfo hints;

    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        return 1;
    }

    ZeroMemory(&hints, sizeof(hints));      // nastaven� winsock socketu
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result); // nastaven� serveru
    if (iResult != 0) {
        printf("getaddrinfo failed with error: %d\n", iResult);
        WSACleanup();
        return 1;
    }

    ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol); // nastaven� serveru
    if (ListenSocket == INVALID_SOCKET) {
        printf("socket failed with error: %ld\n", WSAGetLastError());
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }

    iResult = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);     // nastaven� serveru
    if (iResult == SOCKET_ERROR) {
        printf("bind failed with error: %d\n", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(ListenSocket);
        WSACleanup();
        return 1;
    }

    freeaddrinfo(result);

    iResult = listen(ListenSocket, SOMAXCONN);          // nastaven� serveru
    if (iResult == SOCKET_ERROR) {
        printf("listen failed with error: %d\n", WSAGetLastError());
        closesocket(ListenSocket);
        WSACleanup();
        return 1;
    }

    for (;;)
    {
        SOCKET ClientSocket = accept(ListenSocket, NULL, NULL);
        if (ClientSocket == INVALID_SOCKET) {
            printf("accept failed with error: %d\n", WSAGetLastError());
            closesocket(ListenSocket);
            WSACleanup();
            return 1;
        }
        std::thread t(clientThread, ClientSocket);      // nov� vl�kno, kter� vol� clientThread(ClientSocket)
        t.detach();                                     // osamostatn�n� vl�kna, vl�kno skon�� po dob�hnut� funkce
    }

    closesocket(ListenSocket);      // uzav�en� socketu

    return 0;
}