#undef UNICODE

#define WIN32_LEAN_AND_MEAN             // vylouèení nìkterých hlavièkových souborù pøi windows.h
#define _CRT_SECURE_NO_WARNINGS         // ignorování chyb, zejména pro možnost použít scanf

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <stdio.h>
#include <thread>
#include <string>
#include <sstream>

#pragma comment (lib, "Ws2_32.lib")

#define DEFAULT_BUFLEN 512    // buffer nastaven na 512 bytù
#define DEFAULT_PORT "3999"   // port pro testování aplikace

using namespace std;

class TimeOutException {};  // výjimka pro timeout
class SyntaxError {};       // chyba syntaxe
class LogicError {};        // logická chyba
class OtherError {          // nedefinovaná další chyba, pro konstukci pøedává sting s popisem chyby
    string message;
public:
    OtherError(string message)
        : message(message)
    {}
};
class ClientDisconnectedException {};       // výjimka pøi odpojení robota

void myAssert(bool cond, string message)    // pouze doplòková funkce pro ovìøení návratové hodnoty a vyhožení chyby  
{
    if (!cond)
        throw OtherError(message);
}

string dropLastChars(string input, int lastCount)     // používáno pouze pro oddìlání \a\b z øetìzcù
{
    return input.substr(0, input.length() - lastCount);
}

class Connection // tøída connection je používána jako rozhraní pro odesílání, pøijímání a èetní zpráv
{
private:
    SOCKET clientSocket;
    char recvbuf[DEFAULT_BUFLEN];
    int recvbuflen = DEFAULT_BUFLEN;
    int received = 0;
    int buffPosition = 0;
    char ReadChar()             // funkce pro ètení zpráv, ète se po charakterech zvlášt
    {
        if (buffPosition == received)
        {
            received = recv(clientSocket, recvbuf, recvbuflen, 0);  // recv vrací kladný poèet pro úspìsne pøijetí x bytu, 0 pokud bylo spojeni validne ukonceno, pro dalsi chyby jinak
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
    Connection(const Connection&) = delete;         // tøídu connection nelze kopírovat
public:
    Connection(SOCKET clientSocket)                 // konstruktor connection, pøedává socket klienta
        : clientSocket(clientSocket)
    {}
    void SetTimeOut(int seconds)                    // nastavení timeoutu pro toto spojení
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
    string ReadLine(int max)    // ètení zprávy a její navrácení
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
    void WriteLine(string message)  // odeslání zprávy, zpráva se pøedává jako parametr a pøidávají se ukonèovací znaky
    {
        int iSendResult = send(clientSocket, message.c_str(), message.length(), 0);
        myAssert(iSendResult != SOCKET_ERROR, "send failed with error: " + WSAGetLastError());
        printf("Bytes sent: %d\n", iSendResult);
        iSendResult = send(clientSocket, "\a\b", 2, 0);
        myAssert(iSendResult != SOCKET_ERROR, "send failed with error: " + WSAGetLastError());
        printf("Bytes sent: %d\n", iSendResult);
    }
};

int positivePart(int x) // používáno pro pohyby robota, vrací parametr, pokud je kladný, jinak 0
{
    return max(0, x);
}

int maxIndex(int x0, int x1)    // vrací 0, pokud je x0 vìtší, jinak 1
{
    if (x0 > x1)
        return 0;
    else
        return 1;
}

int maxIndex(int x0, int x1, int x2, int x3)    // tato funkce se používá pro zjištìní smìru, kterým je tøeba se pohybovat k cíli
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

class Robot         // tøída robot reprezentuje již rozhraní samotných 
{
private:
    Connection* connection;     // pøipojení robota pro komunikaci se serverem
    int direction;              // jakým smìrem je robot otoèen
    int x;
    int y;
public:
    Robot(Connection* connection)   // konstruktor pro robota, pøedáváme mu connection jako parametr
        :connection(connection)
    {
        connection->SetTimeOut(1);  // defaultní timeout, ne pøi rechargingu, když neobržíme zprávu od robota
    }
private:
    uint16_t hashString(string input)   // hashovací funkce pro string, používáno pro uživatelké jméno, pøetékání je zaøízeno datovým typem
    {
        unsigned int sum = 0;
        for (int i = 0; i < input.length(); i++)
        {
            sum += (unsigned char)input[i];
        }
        sum *= 1000;
        return sum;
    }
    string readLineAfterRecharging(int max)     // pøes tuhle funkci nejprve ètu zprávy, pøedpokládám, že mùže dojít k dobíjení, protože pøi nìm musím mìnit timeout
    {
        string line = connection->ReadLine(max);    // ètu øádku
        while (line == "RECHARGING")                // pokud se nabijí, pøenastav timeout, èekej na FULL POWER, pokud dodjde jiné než FULL POWER, nastává logická chyba, jinak pøenastav timeout zpìt, pokud se nabijí dál, opakuj, jinak vra další naètenou zprávu
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

    struct Keys {               // struktura autentizaèních klíèù pro pár serveru a klienta
        uint16_t serverKey;
        uint16_t clientKey;
    };

    uint16_t parseNumber(string str)    // pøevod stringu na èíslo
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

    void authentication()       // autentizaèní funkce pro pøihlášení robota pro komunikaci
    {
        Keys keys[5] = {        // páry autentizaèních klíèù
            {23019, 32037},
            {32037, 29295},
            {18789, 13603},
            {16443, 29533},
            {18189, 21952}
        };
        string username = readLineAfterRecharging(20);      // pøeèti užvatelské jméno robota
        connection->WriteLine("107 KEY REQUEST");           // pošlu žádost o klíè
        uint16_t keyID = parseNumber(readLineAfterRecharging(5));   // pøeèti klíè a pøeveï ho na uint16_t
        if (keyID < 0 || keyID >= 5)                            // ovìøení, jestli je klíè v rozsahu
        {
            connection->WriteLine("303 KEY OUT OF RANGE");
            throw OtherError("key range");
        }
        Keys keyPair = keys[keyID];         // pøirazení páru klíèi
        uint16_t hash = hashString(username);   // zahashuj uzivatelske jmeno
        uint16_t confirmation1 = hash + keyPair.serverKey;  // potvrzení serveru
        uint16_t confirmation2expected = hash + keyPair.clientKey;  // oèekávané potvrzení od robota 
        connection->WriteLine(to_string(confirmation1));        // odešli potvrzení robotovi
        uint16_t confirmation2 = parseNumber(readLineAfterRecharging(7));   // pøeèti potvrzení od robota
        if (confirmation2 != confirmation2expected)     // porovnej jestli sedí 
        {
            connection->WriteLine("300 LOGIN FAILED");
            throw OtherError("");
        }
        connection->WriteLine("200 OK");        // úspìšné pøipojení 
    }
    void setDirection(int dx, int dy)       // nastavení smìru robota
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
    void readCoordinates()      // pøeètení souøadnic robota po OK zprávì
    {
        string line = readLineAfterRecharging(12);
        int n;
        int res = sscanf(line.c_str(), "OK %d %d%n", &x, &y, &n);
        if (res != 2 || n != line.length())
            throw SyntaxError();
    }
    bool move()     // vyvolání pohybu u robota
    {
        connection->WriteLine("102 MOVE");  // pošli zprávu MOVE
        int oldX = x;
        int oldY = y;
        readCoordinates();                  // pøeèti nové souøadnice robota
        if (oldX != x || oldY != y)         // pohnul se?
        {
            setDirection(x - oldX, y - oldY);   // aktualizuj smìr
            return true;
        }
        return false;                       // nepohnul se
    }
    void turnLeft()                 // otoè se do leva
    {
        connection->WriteLine("103 TURN LEFT");
        readCoordinates();          // po otoèení rovnìž èekáme OK
    }
    void turnRight()                // otoè se do prava
    {
        connection->WriteLine("104 TURN RIGHT");
        readCoordinates();          // po otoøení rovnìž èekáme OK
    }
    void turn(int direction)    // zmìna smìru
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
    void goTo(int direction)        // otoè se smìrem direction, jdi, pokud narazíš na pøekážku, pøekonej...
    {
        turn(direction);
        goAndAvoidObstacle();
    }
    int mostNeededDirection()       // funkce pro zjištìní smìru, kterým jsme nejdále k cíli, tedy pùjdeme jím
    {
        return maxIndex(
            positivePart(-y),
            positivePart(-x),
            positivePart(y),
            positivePart(x)
        );
    }
    void goToTarget()       // vypoèti nejvíce potøebný smìr, otoè se jím, jdi, pokud narazíš na pøekážku, otoè se...
    {
        goTo(mostNeededDirection());
    }
    void goToCenterOfCoordinateSystem()     // volej chození, dokud nejsme ve støedu
    {
        do {
            goToTarget();
        } while (x != 0 || y != 0);
    }
    void getMessage()       // dej pøíkaz k vyzvednutí zprávy, pøeèti ji a odhlaš robota
    {
        connection->WriteLine("105 GET MESSAGE");
        readLineAfterRecharging(100);
        connection->WriteLine("106 LOGOUT");
    }
public:
    void readLoop()     // øídící funkce pro fáze robota, nejdøíve proveï autentizaci, poté dojdi do støedu, vyzvedni zprávu
    {
        authentication();
        goToCenterOfCoordinateSystem();
        getMessage();
    }
};

int clientThread(SOCKET ClientSocket)       // funkce pro inicializaci pøipojení, pøiøazení robotovi, dále se volá readLoop, který øídí robota a snažíme se odchytit výjimky 
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
    printf("Connection closing...\n");              // øádné ukonèení spojení 
    int iResult = shutdown(ClientSocket, SD_SEND);
    if (iResult == SOCKET_ERROR) {
        printf("shutdown failed with error: %d\n", WSAGetLastError());
        closesocket(ClientSocket);
        return 1;
    }

    closesocket(ClientSocket);          // uzavøení socketu

    printf("Thread ended...\n");        // vlákno konèí
}

int __cdecl main(void)
{
    // https://docs.microsoft.com/en-us/windows/win32/winsock/complete-server-code
    WSADATA wsaData;
    int iResult;

    SOCKET ListenSocket = INVALID_SOCKET;   // nastavení winsock socketu

    struct addrinfo* result = NULL;
    struct addrinfo hints;

    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        return 1;
    }

    ZeroMemory(&hints, sizeof(hints));      // nastavení winsock socketu
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result); // nastavení serveru
    if (iResult != 0) {
        printf("getaddrinfo failed with error: %d\n", iResult);
        WSACleanup();
        return 1;
    }

    ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol); // nastavení serveru
    if (ListenSocket == INVALID_SOCKET) {
        printf("socket failed with error: %ld\n", WSAGetLastError());
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }

    iResult = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);     // nastavení serveru
    if (iResult == SOCKET_ERROR) {
        printf("bind failed with error: %d\n", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(ListenSocket);
        WSACleanup();
        return 1;
    }

    freeaddrinfo(result);

    iResult = listen(ListenSocket, SOMAXCONN);          // nastavení serveru
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
        std::thread t(clientThread, ClientSocket);      // nové vlákno, které volá clientThread(ClientSocket)
        t.detach();                                     // osamostatnìní vlákna, vlákno skonèí po dobìhnutí funkce
    }

    closesocket(ListenSocket);      // uzavøení socketu

    return 0;
}