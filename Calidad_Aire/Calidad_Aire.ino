#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <sys/time.h>
#include <SensirionI2cScd4x.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <float.h>

#define SDA_PIN 8
#define SCL_PIN 9

// Pantalla redonda GC9A01 (SPI; SDA de la pantalla equivale a MOSI).
#define TFT_SCLK 10
#define TFT_MOSI 11
#define TFT_DC   12
#define TFT_CS   13
#define TFT_RST  14

// MicroSD: comparte SCLK y MOSI con la TFT, pero tiene su propio CS.
#define SD_CS    15
#define SD_MISO  16
#define ARCHIVO_DATOS_30S "/calidad_aire_30s.csv"

#define INTERVALO_GUARDADO_SEG 30UL
#define RESUMEN_MAGIC 0x43413131UL

// Sensor de particulas Plantower PMSx003/PMS5003 por UART.
// Conectar TX del sensor a PM_RX_PIN y RX del sensor a PM_TX_PIN.
#define PM_RX_PIN 17
#define PM_TX_PIN 18
#define PM_BAUD   9600
#define PM_VIGENCIA_MS 60000UL
#define PM_GRACIA_INICIAL_MS 15000UL

// Vigencias usadas solamente para el diagnostico de salud por BLE.
#define SCD_VIGENCIA_MS 15000UL
#define SD_VIGENCIA_ESCRITURA_MS \
    ((INTERVALO_GUARDADO_SEG + 15UL) * 1000UL)
#define INTERVALO_ESTADO_BLE_MS 2000UL

const char* ZONA_HORARIA_CHILE = "CLT4CLST,M9.1.0/0,M4.1.0/0";

// Servicio BLE consumido por la aplicacion Android.
#define BLE_NOMBRE "CalidadAire-S3"
#define BLE_SERVICIO_UUID "e4f14c00-7f7a-4a50-8c1a-3c1d2e3f4001"
#define BLE_DATOS_UUID    "e4f14c01-7f7a-4a50-8c1a-3c1d2e3f4001"
#define BLE_PM_UUID       "e4f14c02-7f7a-4a50-8c1a-3c1d2e3f4001"
#define BLE_COMANDO_UUID  "e4f14c03-7f7a-4a50-8c1a-3c1d2e3f4001"
#define BLE_ESTADO_UUID   "e4f14c05-7f7a-4a50-8c1a-3c1d2e3f4001"

#ifdef NO_ERROR
#undef NO_ERROR
#endif

#define NO_ERROR 0
#define COLOR_PM25 0xFD20

SensirionI2cScd4x sensor;
Adafruit_GC9A01A pantalla(TFT_CS, TFT_DC, TFT_RST);
HardwareSerial sensorPM(1);

static char errorMessage[64];
static int16_t error;

bool sensorIniciado = false;
bool sdIniciada = false;
bool horaSincronizada = false;
bool sensorPMIniciado = false;
bool falloSCDActivo = false;
bool falloEscrituraSDActivo = false;
bool escrituraSDConfirmada = false;
bool primeraMedicionSCDRecibida = false;
unsigned long inicioSensorPMMs = 0;
unsigned long inicioEsperaSCDMs = 0;
unsigned long ultimaMedicionSCDMs = 0;
unsigned long ultimaLecturaSCDValidaMs = 0;
unsigned long ultimaEscrituraSDMs = 0;
unsigned long ultimoAvisoEsperaSCDMs = 0;
uint8_t reintentosMedicionSCD = 0;
uint8_t erroresI2CSCDConsecutivos = 0;

struct LecturaPM {
    uint16_t pm25 = 0;
    uint16_t pm10 = 0;
    bool valida = false;
    unsigned long actualizadaMs = 0;
};

struct RegistroResumen {
    uint32_t magic;
    uint32_t intervalo;
    uint32_t inicio;
    uint32_t cantidad;
    uint32_t buenas;
    uint32_t moderadas;
    uint32_t deficientes;
    float sumaCo2;
    float minimoCo2;
    float maximoCo2;
    float sumaTemperatura;
    float minimaTemperatura;
    float maximaTemperatura;
    float sumaHumedad;
    float minimaHumedad;
    float maximaHumedad;
    uint32_t cantidadPM25;
    float sumaPM25;
    float minimoPM25;
    float maximoPM25;
    uint32_t cantidadPM10;
    float sumaPM10;
    float minimoPM10;
    float maximoPM10;
};

LecturaPM ultimaPM;
RegistroResumen acumuladorSD;
bool acumuladorSDActivo = false;
unsigned long inicioAcumuladorSDMs = 0;
BLECharacteristic* caracteristicaDatosBLE = nullptr;
BLECharacteristic* caracteristicaPMBLE = nullptr;
BLECharacteristic* caracteristicaEstadoBLE = nullptr;
volatile bool clienteBLEConectado = false;
volatile bool avisoHoraMovilPendiente = false;

bool lecturaPMDisponible();

class CallbacksComandosBLE : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* caracteristica) override {
        String comando = caracteristica->getValue().c_str();
        comando.trim();

        long long epoch = 0;
        if (sscanf(comando.c_str(), "T|%lld", &epoch) == 1 && epoch > 1700000000LL) {
            struct timeval ahora = { (time_t)epoch, 0 };
            settimeofday(&ahora, nullptr);
            setenv("TZ", ZONA_HORARIA_CHILE, 1);
            tzset();
            horaSincronizada = true;
            avisoHoraMovilPendiente = true;
            Serial.println("Fecha y hora recibidas desde la aplicacion Android.");
            return;
        }

        // La app conserva su propio historial. La microSD no se consulta por
        // BLE; este canal se utiliza exclusivamente para ajustar la hora.
    }
};

class CallbacksServidorBLE : public BLEServerCallbacks {
    void onConnect(BLEServer* servidor) override {
        clienteBLEConectado = true;
        Serial.println("Aplicacion Bluetooth conectada.");
    }

    void onDisconnect(BLEServer* servidor) override {
        clienteBLEConectado = false;
        Serial.println("Aplicacion Bluetooth desconectada.");
        BLEDevice::startAdvertising();
    }
};

void iniciarBluetooth() {
    BLEDevice::init(BLE_NOMBRE);
    Serial.print("Direccion MAC BLE: ");
    Serial.println(BLEDevice::getAddress().toString().c_str());
    BLEServer* servidor = BLEDevice::createServer();
    servidor->setCallbacks(new CallbacksServidorBLE());
    BLEService* servicio = servidor->createService(BLE_SERVICIO_UUID);

    caracteristicaDatosBLE = servicio->createCharacteristic(
        BLE_DATOS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    caracteristicaDatosBLE->addDescriptor(new BLE2902());
    caracteristicaDatosBLE->setValue("---|--.-|--.-|-");

    caracteristicaPMBLE = servicio->createCharacteristic(
        BLE_PM_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    caracteristicaPMBLE->addDescriptor(new BLE2902());
    caracteristicaPMBLE->setValue("--|--");

    // Canal opcional e independiente: no modifica los payloads historicos de
    // ambiente ni particulas. 0=error, 1=OK, 2=iniciando/esperando.
    caracteristicaEstadoBLE = servicio->createCharacteristic(
        BLE_ESTADO_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    caracteristicaEstadoBLE->addDescriptor(new BLE2902());
    caracteristicaEstadoBLE->setValue("H|2|2|2|-1");

    BLECharacteristic* caracteristicaComando = servicio->createCharacteristic(
        BLE_COMANDO_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    caracteristicaComando->setCallbacks(new CallbacksComandosBLE());

    servicio->start();

    BLEAdvertising* publicidad = BLEDevice::getAdvertising();
    publicidad->addServiceUUID(BLE_SERVICIO_UUID);
    publicidad->setScanResponse(true);
    BLEDevice::startAdvertising();
    Serial.println("Bluetooth BLE listo: CalidadAire-S3");
}

void enviarMedicionBluetooth(uint16_t co2, float temperatura, float humedad) {
    if (!clienteBLEConectado || caracteristicaDatosBLE == nullptr ||
        caracteristicaPMBLE == nullptr) {
        return;
    }

    char estado = co2 < 800 ? 'B' : (co2 <= 1000 ? 'M' : 'D');
    char datosAmbiente[20];
    snprintf(datosAmbiente, sizeof(datosAmbiente), "%u|%.1f|%.1f|%c",
             co2, temperatura, humedad, estado);
    caracteristicaDatosBLE->setValue(datosAmbiente);
    caracteristicaDatosBLE->notify();

    char datosParticulas[16];
    if (lecturaPMDisponible()) {
        snprintf(datosParticulas, sizeof(datosParticulas), "%u|%u",
                 ultimaPM.pm25, ultimaPM.pm10);
    } else {
        snprintf(datosParticulas, sizeof(datosParticulas), "--|--");
    }
    caracteristicaPMBLE->setValue(datosParticulas);
    caracteristicaPMBLE->notify();

    Serial.print("BLE enviado: ");
    Serial.print(datosAmbiente);
    Serial.print(" | PM: ");
    Serial.println(datosParticulas);
}

uint16_t leerUint16BE(const uint8_t* datos, uint8_t indice) {
    return ((uint16_t)datos[indice] << 8) | datos[indice + 1];
}

bool lecturaPMDisponible() {
    return ultimaPM.valida &&
           millis() - ultimaPM.actualizadaMs < PM_VIGENCIA_MS;
}

uint8_t estadoSaludSCD(unsigned long ahoraMs) {
    if (!sensorIniciado || falloSCDActivo) {
        return 0;
    }
    if (!primeraMedicionSCDRecibida) {
        return 2;
    }
    return ultimaLecturaSCDValidaMs != 0 &&
           ahoraMs - ultimaLecturaSCDValidaMs < SCD_VIGENCIA_MS ? 1 : 0;
}

uint8_t estadoSaludPM(unsigned long ahoraMs) {
    if (!sensorPMIniciado) {
        return 0;
    }
    if (lecturaPMDisponible()) {
        return 1;
    }
    if (!ultimaPM.valida &&
        ahoraMs - inicioSensorPMMs < PM_GRACIA_INICIAL_MS) {
        return 2;
    }
    return 0;
}

uint8_t estadoSaludSD(unsigned long ahoraMs) {
    if (!sdIniciada || falloEscrituraSDActivo) {
        return 0;
    }
    if (!escrituraSDConfirmada) {
        return 2;
    }
    return ahoraMs - ultimaEscrituraSDMs < SD_VIGENCIA_ESCRITURA_MS ? 1 : 0;
}

void publicarEstadoBluetooth() {
    if (caracteristicaEstadoBLE == nullptr) {
        return;
    }

    static bool primeraPublicacion = true;
    static unsigned long ultimaPublicacionMs = 0;
    unsigned long ahoraMs = millis();
    if (!primeraPublicacion &&
        ahoraMs - ultimaPublicacionMs < INTERVALO_ESTADO_BLE_MS) {
        return;
    }
    primeraPublicacion = false;
    ultimaPublicacionMs = ahoraMs;

    long edadUltimaEscrituraSeg = escrituraSDConfirmada
            ? (long)((ahoraMs - ultimaEscrituraSDMs) / 1000UL)
            : -1L;
    char mensaje[32];
    snprintf(mensaje, sizeof(mensaje), "H|%u|%u|%u|%ld",
             (unsigned int)estadoSaludSCD(ahoraMs),
             (unsigned int)estadoSaludPM(ahoraMs),
             (unsigned int)estadoSaludSD(ahoraMs),
             edadUltimaEscrituraSeg);
    caracteristicaEstadoBLE->setValue(mensaje);
    if (clienteBLEConectado) {
        caracteristicaEstadoBLE->notify();
    }
}

bool actualizarLecturaPM() {
    static uint8_t trama[32];
    static uint8_t indice = 0;
    bool nuevaLectura = false;

    while (sensorPM.available()) {
        uint8_t byteRecibido = sensorPM.read();

        if (indice == 0 && byteRecibido != 0x42) {
            continue;
        }
        if (indice == 1 && byteRecibido != 0x4D) {
            indice = (byteRecibido == 0x42) ? 1 : 0;
            trama[0] = 0x42;
            continue;
        }

        trama[indice++] = byteRecibido;

        if (indice < sizeof(trama)) {
            continue;
        }

        indice = 0;

        if (leerUint16BE(trama, 2) != 28) {
            continue;
        }

        uint16_t checksumCalculado = 0;
        for (uint8_t i = 0; i < 30; i++) {
            checksumCalculado += trama[i];
        }

        if (checksumCalculado != leerUint16BE(trama, 30)) {
            continue;
        }

        // Valores atmosfericos: los mas utiles para aire ambiente.
        ultimaPM.pm25 = leerUint16BE(trama, 12);
        ultimaPM.pm10 = leerUint16BE(trama, 14);
        ultimaPM.valida = true;
        ultimaPM.actualizadaMs = millis();
        nuevaLectura = true;
    }

    return nuevaLectura;
}

// CO2 se usa como indicador de ventilacion, no como medicion completa de
// contaminantes del aire. Rangos basados en la guia de ventilacion de CDC/NIOSH.
const char* clasificarVentilacion(uint16_t co2) {
    if (co2 < 800) {
        return "BUENA";
    }
    if (co2 <= 1000) {
        return "MODERADA";
    }
    return "DEFICIENTE";
}

uint16_t colorClasificacion(uint16_t co2) {
    if (co2 < 800) {
        return GC9A01A_GREEN;
    }
    if (co2 <= 1000) {
        return GC9A01A_YELLOW;
    }
    return GC9A01A_RED;
}

void obtenerFechaHora(char* destino, size_t largo) {
    if (!horaSincronizada) {
        snprintf(destino, largo, "SIN_HORA");
        return;
    }
    time_t ahora = time(nullptr);
    struct tm* fechaLocal = localtime(&ahora);
    strftime(destino, largo, "%Y-%m-%d %H:%M:%S", fechaLocal);
}

void registrarMedicionSD(uint16_t co2, float temperatura, float humedad) {
    if (!sdIniciada) {
        return;
    }

    if (!acumuladorSDActivo) {
        reiniciarRegistroResumen(acumuladorSD, INTERVALO_GUARDADO_SEG, 0);
        acumuladorSDActivo = true;
        inicioAcumuladorSDMs = millis();
    }

    bool pmValida = lecturaPMDisponible();
    agregarMedicionResumen(acumuladorSD, co2, temperatura, humedad,
                           pmValida ? ultimaPM.pm25 : -1,
                           pmValida ? ultimaPM.pm10 : -1);

    if (millis() - inicioAcumuladorSDMs <
        INTERVALO_GUARDADO_SEG * 1000UL) {
        return;
    }

    File archivo = SD.open(ARCHIVO_DATOS_30S, FILE_APPEND);
    if (!archivo) {
        falloEscrituraSDActivo = true;
        Serial.println("Error: no se pudo guardar el promedio de 30 segundos.");
        // El acumulado permanece en RAM para reintentar en la proxima lectura.
        return;
    }

    char fechaHora[20];
    obtenerFechaHora(fechaHora, sizeof(fechaHora));
    archivo.print(fechaHora);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidad);
    archivo.print(',');
    archivo.print(acumuladorSD.sumaCo2 / acumuladorSD.cantidad, 1);
    archivo.print(',');
    archivo.print(acumuladorSD.minimoCo2, 0);
    archivo.print(',');
    archivo.print(acumuladorSD.maximoCo2, 0);
    archivo.print(',');
    archivo.print(acumuladorSD.sumaTemperatura / acumuladorSD.cantidad, 2);
    archivo.print(',');
    archivo.print(acumuladorSD.sumaHumedad / acumuladorSD.cantidad, 2);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidadPM25);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidadPM25
            ? acumuladorSD.sumaPM25 / acumuladorSD.cantidadPM25 : -1, 1);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidadPM25 ? acumuladorSD.minimoPM25 : -1, 0);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidadPM25 ? acumuladorSD.maximoPM25 : -1, 0);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidadPM10);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidadPM10
            ? acumuladorSD.sumaPM10 / acumuladorSD.cantidadPM10 : -1, 1);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidadPM10 ? acumuladorSD.minimoPM10 : -1, 0);
    archivo.print(',');
    archivo.print(acumuladorSD.cantidadPM10 ? acumuladorSD.maximoPM10 : -1, 0);
    archivo.print(',');
    archivo.print(acumuladorSD.buenas);
    archivo.print(',');
    archivo.print(acumuladorSD.moderadas);
    archivo.print(',');
    archivo.print(acumuladorSD.deficientes);
    archivo.println();
    archivo.flush();
    bool escrituraCorrecta = archivo.getWriteError() == 0;
    archivo.close();

    if (!escrituraCorrecta) {
        falloEscrituraSDActivo = true;
        Serial.println("Error durante la escritura del promedio; se reintentara.");
        return;
    }

    falloEscrituraSDActivo = false;
    escrituraSDConfirmada = true;
    ultimaEscrituraSDMs = millis();

    Serial.print("Promedio de ");
    Serial.print(acumuladorSD.cantidad);
    Serial.println(" mediciones guardado en la microSD.");
    if (acumuladorSD.cantidadPM25 > 0 && acumuladorSD.cantidadPM10 > 0) {
        Serial.print("CSV PM2.5 promedio: ");
        Serial.print(acumuladorSD.sumaPM25 / acumuladorSD.cantidadPM25, 1);
        Serial.print(" (");
        Serial.print(acumuladorSD.cantidadPM25);
        Serial.print(" muestras) | PM10 promedio: ");
        Serial.print(acumuladorSD.sumaPM10 / acumuladorSD.cantidadPM10, 1);
        Serial.print(" (");
        Serial.print(acumuladorSD.cantidadPM10);
        Serial.println(" muestras)");
    } else {
        Serial.println(
            "CSV sin PM: no llego una trama UART valida durante la ventana."
        );
    }
    reiniciarRegistroResumen(acumuladorSD, INTERVALO_GUARDADO_SEG, 0);
    acumuladorSDActivo = true;
    inicioAcumuladorSDMs = millis();
}

#if 0
// Código histórico de lectura/indexación de microSD, desactivado. La tarjeta
// se usa únicamente como destino de escritura; los reportes viven en Android.
void notificarReporte(const char* mensaje) {
    if (!clienteBLEConectado || caracteristicaReporteBLE == nullptr) {
        return;
    }
    caracteristicaReporteBLE->setValue(mensaje);
    caracteristicaReporteBLE->notify();
    // Las notificaciones se espacian para no saturar la cola BLE del telefono.
    delay(24);
}

bool convertirFechaCSV(const char* texto, time_t& epoch) {
    int ano, mes, dia, hora, minuto, segundo;
    if (sscanf(texto, "%d-%d-%d %d:%d:%d",
               &ano, &mes, &dia, &hora, &minuto, &segundo) != 6) {
        return false;
    }

    struct tm fecha = {};
    fecha.tm_year = ano - 1900;
    fecha.tm_mon = mes - 1;
    fecha.tm_mday = dia;
    fecha.tm_hour = hora;
    fecha.tm_min = minuto;
    fecha.tm_sec = segundo;
    fecha.tm_isdst = -1;
    epoch = mktime(&fecha);
    return epoch != (time_t)-1;
}
#endif

void reiniciarRegistroResumen(RegistroResumen& registro,
                              uint32_t intervalo, uint32_t inicio) {
    memset(&registro, 0, sizeof(registro));
    registro.magic = RESUMEN_MAGIC;
    registro.intervalo = intervalo;
    registro.inicio = inicio;
    registro.minimoCo2 = FLT_MAX;
    registro.minimaTemperatura = FLT_MAX;
    registro.maximaTemperatura = -FLT_MAX;
    registro.minimaHumedad = FLT_MAX;
    registro.maximaHumedad = -FLT_MAX;
    registro.minimoPM25 = FLT_MAX;
    registro.minimoPM10 = FLT_MAX;
}

void agregarMedicionResumen(RegistroResumen& registro, uint16_t co2,
                            float temperatura, float humedad,
                            int pm25, int pm10) {
    registro.cantidad++;
    registro.sumaCo2 += co2;
    registro.minimoCo2 = min(registro.minimoCo2, (float)co2);
    registro.maximoCo2 = max(registro.maximoCo2, (float)co2);
    registro.sumaTemperatura += temperatura;
    registro.minimaTemperatura = min(registro.minimaTemperatura, temperatura);
    registro.maximaTemperatura = max(registro.maximaTemperatura, temperatura);
    registro.sumaHumedad += humedad;
    registro.minimaHumedad = min(registro.minimaHumedad, humedad);
    registro.maximaHumedad = max(registro.maximaHumedad, humedad);

    if (co2 < 800) {
        registro.buenas++;
    } else if (co2 <= 1000) {
        registro.moderadas++;
    } else {
        registro.deficientes++;
    }

    if (pm25 >= 0) {
        registro.cantidadPM25++;
        registro.sumaPM25 += pm25;
        registro.minimoPM25 = min(registro.minimoPM25, (float)pm25);
        registro.maximoPM25 = max(registro.maximoPM25, (float)pm25);
    }
    if (pm10 >= 0) {
        registro.cantidadPM10++;
        registro.sumaPM10 += pm10;
        registro.minimoPM10 = min(registro.minimoPM10, (float)pm10);
        registro.maximoPM10 = max(registro.maximoPM10, (float)pm10);
    }
}

#if 0
bool escribirRegistroResumen(File& archivo, const RegistroResumen& registro) {
    return archivo.write((const uint8_t*)&registro, sizeof(registro)) ==
           sizeof(registro);
}

bool anexarRegistroResumen(const char* ruta, const RegistroResumen& registro) {
    if (!registroResumenValido(registro, registro.intervalo)) {
        return false;
    }
    File archivo = SD.open(ruta, FILE_APPEND);
    if (!archivo) {
        return false;
    }
    bool escrito = escribirRegistroResumen(archivo, registro);
    archivo.close();
    return escrito;
}

bool leerLineaMedicion(const String& linea, time_t& instante,
                       uint16_t& co2, float& temperatura, float& humedad,
                       int& pm25, int& pm10) {
    if (linea.length() < 20 || linea.startsWith("fecha_hora") ||
        linea.startsWith("SIN_HORA")) {
        return false;
    }

    char fechaTexto[20] = {};
    unsigned int co2Leido = 0;
    pm25 = -1;
    pm10 = -1;
    int campos = sscanf(linea.c_str(), "%19[^,],%u,%f,%f,%d,%d",
                        fechaTexto, &co2Leido, &temperatura, &humedad,
                        &pm25, &pm10);
    if (campos < 4 || co2Leido > UINT16_MAX ||
        !convertirFechaCSV(fechaTexto, instante)) {
        return false;
    }
    co2 = (uint16_t)co2Leido;
    return true;
}

bool leerLineaResumen30s(const String& linea, time_t& instante,
                         RegistroResumen& lote) {
    if (linea.length() < 20 || linea.startsWith("fecha_hora") ||
        linea.startsWith("SIN_HORA")) {
        return false;
    }

    char fechaTexto[20] = {};
    unsigned int muestras = 0;
    unsigned int muestrasPM25 = 0;
    unsigned int muestrasPM10 = 0;
    unsigned int buenas = 0;
    unsigned int moderadas = 0;
    unsigned int deficientes = 0;
    float co2Promedio, co2Minimo, co2Maximo;
    float temperaturaPromedio, humedadPromedio;
    float pm25Promedio, pm25Minimo, pm25Maximo;
    float pm10Promedio, pm10Minimo, pm10Maximo;

    int campos = sscanf(
        linea.c_str(),
        "%19[^,],%u,%f,%f,%f,%f,%f,%u,%f,%f,%f,%u,%f,%f,%f,%u,%u,%u",
        fechaTexto, &muestras, &co2Promedio, &co2Minimo, &co2Maximo,
        &temperaturaPromedio, &humedadPromedio,
        &muestrasPM25, &pm25Promedio, &pm25Minimo, &pm25Maximo,
        &muestrasPM10, &pm10Promedio, &pm10Minimo, &pm10Maximo,
        &buenas, &moderadas, &deficientes
    );
    if (campos != 18 || muestras == 0 ||
        !convertirFechaCSV(fechaTexto, instante)) {
        return false;
    }

    reiniciarRegistroResumen(lote, INTERVALO_GUARDADO_SEG,
                             (uint32_t)instante);
    lote.cantidad = muestras;
    lote.buenas = buenas;
    lote.moderadas = moderadas;
    lote.deficientes = deficientes;
    lote.sumaCo2 = co2Promedio * muestras;
    lote.minimoCo2 = co2Minimo;
    lote.maximoCo2 = co2Maximo;
    lote.sumaTemperatura = temperaturaPromedio * muestras;
    lote.minimaTemperatura = temperaturaPromedio;
    lote.maximaTemperatura = temperaturaPromedio;
    lote.sumaHumedad = humedadPromedio * muestras;
    lote.minimaHumedad = humedadPromedio;
    lote.maximaHumedad = humedadPromedio;
    lote.cantidadPM25 = muestrasPM25;
    lote.sumaPM25 = muestrasPM25 ? pm25Promedio * muestrasPM25 : 0;
    lote.minimoPM25 = pm25Minimo;
    lote.maximoPM25 = pm25Maximo;
    lote.cantidadPM10 = muestrasPM10;
    lote.sumaPM10 = muestrasPM10 ? pm10Promedio * muestrasPM10 : 0;
    lote.minimoPM10 = pm10Minimo;
    lote.maximoPM10 = pm10Maximo;
    return true;
}

void agregarAIndice(File& archivo, RegistroResumen& actual, bool& activo,
                    uint32_t intervalo, time_t instante, uint16_t co2,
                    float temperatura, float humedad, int pm25, int pm10) {
    uint32_t inicio = ((uint32_t)instante / intervalo) * intervalo;
    if (!activo || actual.inicio != inicio) {
        if (activo) {
            escribirRegistroResumen(archivo, actual);
        }
        reiniciarRegistroResumen(actual, intervalo, inicio);
        activo = true;
    }
    agregarMedicionResumen(actual, co2, temperatura, humedad, pm25, pm10);
}

void agregarLoteAIndice(File& archivo, RegistroResumen& actual, bool& activo,
                        uint32_t intervalo, time_t instante,
                        const RegistroResumen& lote) {
    uint32_t inicio = ((uint32_t)instante / intervalo) * intervalo;
    if (!activo || actual.inicio != inicio) {
        if (activo) {
            escribirRegistroResumen(archivo, actual);
        }
        reiniciarRegistroResumen(actual, intervalo, inicio);
        activo = true;
    }
    fusionarRegistro(actual, lote);
}

bool archivosResumenValidos() {
    if (!SD.exists(MARCADOR_RESUMEN) ||
        !SD.exists(ARCHIVO_RESUMEN_15M) ||
        !SD.exists(ARCHIVO_RESUMEN_1H)) {
        return false;
    }
    File resumen15 = SD.open(ARCHIVO_RESUMEN_15M, FILE_READ);
    File resumen1h = SD.open(ARCHIVO_RESUMEN_1H, FILE_READ);
    bool validos = resumen15 && resumen1h &&
            resumen15.size() % sizeof(RegistroResumen) == 0 &&
            resumen1h.size() % sizeof(RegistroResumen) == 0;
    if (resumen15) resumen15.close();
    if (resumen1h) resumen1h.close();
    return validos;
}

bool construirIndicesResumen() {
    if (!sdIniciada) {
        return false;
    }
    if (archivosResumenValidos()) {
        Serial.println("Indices de reporte encontrados; no se reconstruyen.");
        return true;
    }

    Serial.println("Construyendo indices de reportes por unica vez...");
    mostrarEstadoConexion("MICROSD", "INDEXANDO", "Preparando reportes", GC9A01A_BLUE);
    setenv("TZ", ZONA_HORARIA_CHILE, 1);
    tzset();

    SD.remove(MARCADOR_RESUMEN);
    SD.remove(ARCHIVO_RESUMEN_15M);
    SD.remove(ARCHIVO_RESUMEN_1H);

    File indice15 = SD.open(ARCHIVO_RESUMEN_15M, FILE_APPEND);
    File indice1h = SD.open(ARCHIVO_RESUMEN_1H, FILE_APPEND);
    if (!indice15 || !indice1h) {
        if (indice15) indice15.close();
        if (indice1h) indice1h.close();
        Serial.println("No se pudieron crear los indices de la microSD.");
        return false;
    }

    RegistroResumen actual15;
    RegistroResumen actual1h;
    bool activo15 = false;
    bool activo1h = false;
    uint32_t filasValidas = 0;

    File datosAnteriores = SD.open(ARCHIVO_DATOS, FILE_READ);
    if (datosAnteriores) {
        while (datosAnteriores.available()) {
            String linea = datosAnteriores.readStringUntil('\n');
            linea.trim();
            time_t instante = 0;
            uint16_t co2 = 0;
            float temperatura = 0;
            float humedad = 0;
            int pm25 = -1;
            int pm10 = -1;
            if (!leerLineaMedicion(linea, instante, co2, temperatura,
                                   humedad, pm25, pm10)) {
                continue;
            }

            agregarAIndice(indice15, actual15, activo15, INTERVALO_15M,
                           instante, co2, temperatura, humedad, pm25, pm10);
            agregarAIndice(indice1h, actual1h, activo1h, INTERVALO_1H,
                           instante, co2, temperatura, humedad, pm25, pm10);
            filasValidas++;
            if (filasValidas % 5000 == 0) {
                Serial.print("Filas indexadas: ");
                Serial.println(filasValidas);
                char progreso[24];
                int porcentaje = datosAnteriores.size() > 0
                        ? (int)(datosAnteriores.position() * 100ULL /
                                datosAnteriores.size()) : 100;
                snprintf(progreso, sizeof(progreso), "%d%% del historial",
                         porcentaje);
                mostrarEstadoConexion("MICROSD", "INDEXANDO", progreso,
                                      GC9A01A_BLUE);
                yield();
            }
        }
        datosAnteriores.close();
    }

    File datos30s = SD.open(ARCHIVO_DATOS_30S, FILE_READ);
    if (datos30s) {
        while (datos30s.available()) {
            String linea = datos30s.readStringUntil('\n');
            linea.trim();
            time_t instante = 0;
            RegistroResumen lote;
            if (!leerLineaResumen30s(linea, instante, lote)) {
                continue;
            }
            agregarLoteAIndice(indice15, actual15, activo15, INTERVALO_15M,
                               instante, lote);
            agregarLoteAIndice(indice1h, actual1h, activo1h, INTERVALO_1H,
                               instante, lote);
            filasValidas++;
        }
        datos30s.close();
    }
    if (activo15) escribirRegistroResumen(indice15, actual15);
    if (activo1h) escribirRegistroResumen(indice1h, actual1h);
    indice15.close();
    indice1h.close();

    File marcador = SD.open(MARCADOR_RESUMEN, FILE_WRITE);
    if (!marcador) {
        Serial.println("No se pudo confirmar la creacion de los indices.");
        return false;
    }
    marcador.print("v1");
    marcador.close();
    resumen15Activo = false;
    resumen1hActivo = false;
    Serial.print("Indices listos. Filas historicas procesadas: ");
    Serial.println(filasValidas);
    mostrarEstadoConexion("MICROSD", "INDICE LISTO", "Reportes optimizados", GC9A01A_GREEN);
    return true;
}

void agregarSoloAlUltimoBloque(RegistroResumen& actual, bool& activo,
                               uint32_t intervalo, time_t instante,
                               uint16_t co2, float temperatura, float humedad,
                               int pm25, int pm10) {
    uint32_t inicio = ((uint32_t)instante / intervalo) * intervalo;
    if (!activo || inicio > actual.inicio) {
        reiniciarRegistroResumen(actual, intervalo, inicio);
        activo = true;
    }
    if (inicio == actual.inicio) {
        agregarMedicionResumen(actual, co2, temperatura, humedad, pm25, pm10);
    }
}

void fusionarSoloAlUltimoBloque(RegistroResumen& actual, bool& activo,
                                uint32_t intervalo, time_t instante,
                                const RegistroResumen& lote) {
    uint32_t inicio = ((uint32_t)instante / intervalo) * intervalo;
    if (!activo || inicio > actual.inicio) {
        reiniciarRegistroResumen(actual, intervalo, inicio);
        activo = true;
    }
    if (inicio == actual.inicio) {
        fusionarRegistro(actual, lote);
    }
}

void reconstruirBloquesActualesDesdeCSV() {
    if (!sdIniciada) {
        return;
    }

    // Solo se examina la cola del CSV. Es suficiente para reconstruir el
    // bloque actual de una hora tras un reinicio, sin recorrer el historial.
    const uint32_t bytesCola = 128UL * 1024UL;
    resumen15Activo = false;
    resumen1hActivo = false;
    uint32_t filas = 0;

    File datosAnteriores = SD.open(ARCHIVO_DATOS, FILE_READ);
    if (datosAnteriores) {
        if (datosAnteriores.size() > bytesCola) {
            datosAnteriores.seek(datosAnteriores.size() - bytesCola);
            datosAnteriores.readStringUntil('\n');
        }
        while (datosAnteriores.available()) {
            String linea = datosAnteriores.readStringUntil('\n');
            linea.trim();
            time_t instante = 0;
            uint16_t co2 = 0;
            float temperatura = 0;
            float humedad = 0;
            int pm25 = -1;
            int pm10 = -1;
            if (!leerLineaMedicion(linea, instante, co2, temperatura,
                                   humedad, pm25, pm10)) {
                continue;
            }
            agregarSoloAlUltimoBloque(resumen15Actual, resumen15Activo,
                                      INTERVALO_15M, instante, co2,
                                      temperatura, humedad, pm25, pm10);
            agregarSoloAlUltimoBloque(resumen1hActual, resumen1hActivo,
                                      INTERVALO_1H, instante, co2,
                                      temperatura, humedad, pm25, pm10);
            filas++;
        }
        datosAnteriores.close();
    }

    File datos30s = SD.open(ARCHIVO_DATOS_30S, FILE_READ);
    if (datos30s) {
        if (datos30s.size() > bytesCola) {
            datos30s.seek(datos30s.size() - bytesCola);
            datos30s.readStringUntil('\n');
        }
        while (datos30s.available()) {
            String linea = datos30s.readStringUntil('\n');
            linea.trim();
            time_t instante = 0;
            RegistroResumen lote;
            if (!leerLineaResumen30s(linea, instante, lote)) {
                continue;
            }
            fusionarSoloAlUltimoBloque(resumen15Actual, resumen15Activo,
                                       INTERVALO_15M, instante, lote);
            fusionarSoloAlUltimoBloque(resumen1hActual, resumen1hActivo,
                                       INTERVALO_1H, instante, lote);
            filas++;
        }
        datos30s.close();
    }
    Serial.print("Cola rapida del CSV revisada. Filas: ");
    Serial.println(filas);
}

bool cargarUltimoRegistro(const char* ruta, uint32_t intervalo,
                          uint32_t inicio, RegistroResumen& destino) {
    File archivo = SD.open(ruta, FILE_READ);
    if (!archivo || archivo.size() < sizeof(RegistroResumen)) {
        if (archivo) archivo.close();
        return false;
    }
    archivo.seek(archivo.size() - sizeof(RegistroResumen));
    bool leido = archivo.read((uint8_t*)&destino, sizeof(destino)) ==
                 sizeof(destino);
    archivo.close();
    return leido && registroResumenValido(destino, intervalo) &&
           destino.inicio == inicio;
}

void actualizarResumenActivo(const char* ruta, RegistroResumen& actual,
                             bool& activo, uint32_t intervalo, time_t instante,
                             uint16_t co2, float temperatura, float humedad,
                             int pm25, int pm10) {
    uint32_t inicio = ((uint32_t)instante / intervalo) * intervalo;
    if (!activo) {
        activo = cargarUltimoRegistro(ruta, intervalo, inicio, actual);
        if (!activo) {
            reiniciarRegistroResumen(actual, intervalo, inicio);
            activo = true;
        }
    } else if (actual.inicio != inicio) {
        if (!anexarRegistroResumen(ruta, actual)) {
            Serial.println("Aviso: no se pudo actualizar un indice de reporte.");
        }
        reiniciarRegistroResumen(actual, intervalo, inicio);
    }
    agregarMedicionResumen(actual, co2, temperatura, humedad, pm25, pm10);
}

void actualizarIndicesConMedicion(uint16_t co2, float temperatura,
                                  float humedad) {
    if (!sdIniciada || !horaSincronizada) {
        return;
    }
    time_t instante = time(nullptr);
    bool pmValida = lecturaPMDisponible();
    int pm25 = pmValida ? ultimaPM.pm25 : -1;
    int pm10 = pmValida ? ultimaPM.pm10 : -1;
    actualizarResumenActivo(ARCHIVO_RESUMEN_15M, resumen15Actual,
                            resumen15Activo, INTERVALO_15M, instante,
                            co2, temperatura, humedad, pm25, pm10);
    actualizarResumenActivo(ARCHIVO_RESUMEN_1H, resumen1hActual,
                            resumen1hActivo, INTERVALO_1H, instante,
                            co2, temperatura, humedad, pm25, pm10);
}

int buscarRegistroPorInicio(RegistroResumen* registros, int cantidad,
                            uint32_t inicio) {
    for (int i = 0; i < cantidad; i++) {
        if (registros[i].inicio == inicio) {
            return i;
        }
    }
    return -1;
}

int leerResumenReciente(const char* ruta, uint32_t intervalo,
                        time_t desde, time_t hasta) {
    File archivo = SD.open(ruta, FILE_READ);
    if (!archivo) {
        return -1;
    }

    uint32_t total = archivo.size() / sizeof(RegistroResumen);
    uint32_t maximoLeer = MAX_PUNTOS_REPORTE + 4;
    uint32_t primero = total > maximoLeer ? total - maximoLeer : 0;
    archivo.seek(primero * sizeof(RegistroResumen));
    int cantidad = 0;

    for (uint32_t i = primero; i < total; i++) {
        RegistroResumen registro;
        if (archivo.read((uint8_t*)&registro, sizeof(registro)) !=
            sizeof(registro)) {
            break;
        }
        if (!registroResumenValido(registro, intervalo) ||
            (time_t)registro.inicio + intervalo <= desde ||
            (time_t)registro.inicio >= hasta) {
            continue;
        }
        int existente = buscarRegistroPorInicio(registrosReporte, cantidad,
                                                registro.inicio);
        if (existente >= 0) {
            registrosReporte[existente] = registro;
        } else if (cantidad < MAX_PUNTOS_REPORTE + 4) {
            registrosReporte[cantidad++] = registro;
        }
    }
    archivo.close();

    RegistroResumen* actual = intervalo == INTERVALO_15M
            ? &resumen15Actual : &resumen1hActual;
    bool activo = intervalo == INTERVALO_15M
            ? resumen15Activo : resumen1hActivo;
    if (activo && registroResumenValido(*actual, intervalo) &&
        (time_t)actual->inicio + intervalo > desde &&
        (time_t)actual->inicio < hasta) {
        int existente = buscarRegistroPorInicio(registrosReporte, cantidad,
                                                actual->inicio);
        if (existente >= 0) {
            registrosReporte[existente] = *actual;
        } else if (cantidad < MAX_PUNTOS_REPORTE + 4) {
            registrosReporte[cantidad++] = *actual;
        }
    }
    return cantidad;
}

void ordenarRegistrosReporte(int cantidad) {
    // Los bloques reconstruidos tras un reinicio pueden quedar anexados fuera
    // de orden en el archivo. Se ordenan antes de calcular y transmitir para
    // impedir que la linea del grafico retroceda en el eje temporal.
    for (int i = 1; i < cantidad; i++) {
        RegistroResumen actual = registrosReporte[i];
        int j = i - 1;
        while (j >= 0 && registrosReporte[j].inicio > actual.inicio) {
            registrosReporte[j + 1] = registrosReporte[j];
            j--;
        }
        registrosReporte[j + 1] = actual;
    }
}

void fusionarRegistro(RegistroResumen& total, const RegistroResumen& origen) {
    if (total.cantidad == 0) {
        total.minimoCo2 = origen.minimoCo2;
        total.maximoCo2 = origen.maximoCo2;
        total.minimaTemperatura = origen.minimaTemperatura;
        total.maximaTemperatura = origen.maximaTemperatura;
        total.minimaHumedad = origen.minimaHumedad;
        total.maximaHumedad = origen.maximaHumedad;
    } else {
        total.minimoCo2 = min(total.minimoCo2, origen.minimoCo2);
        total.maximoCo2 = max(total.maximoCo2, origen.maximoCo2);
        total.minimaTemperatura = min(total.minimaTemperatura,
                                      origen.minimaTemperatura);
        total.maximaTemperatura = max(total.maximaTemperatura,
                                      origen.maximaTemperatura);
        total.minimaHumedad = min(total.minimaHumedad, origen.minimaHumedad);
        total.maximaHumedad = max(total.maximaHumedad, origen.maximaHumedad);
    }
    if (origen.cantidadPM25 > 0) {
        if (total.cantidadPM25 == 0) {
            total.minimoPM25 = origen.minimoPM25;
            total.maximoPM25 = origen.maximoPM25;
        } else {
            total.minimoPM25 = min(total.minimoPM25, origen.minimoPM25);
            total.maximoPM25 = max(total.maximoPM25, origen.maximoPM25);
        }
    }
    if (origen.cantidadPM10 > 0) {
        if (total.cantidadPM10 == 0) {
            total.minimoPM10 = origen.minimoPM10;
            total.maximoPM10 = origen.maximoPM10;
        } else {
            total.minimoPM10 = min(total.minimoPM10, origen.minimoPM10);
            total.maximoPM10 = max(total.maximoPM10, origen.maximoPM10);
        }
    }

    total.cantidad += origen.cantidad;
    total.buenas += origen.buenas;
    total.moderadas += origen.moderadas;
    total.deficientes += origen.deficientes;
    total.sumaCo2 += origen.sumaCo2;
    total.sumaTemperatura += origen.sumaTemperatura;
    total.sumaHumedad += origen.sumaHumedad;
    total.cantidadPM25 += origen.cantidadPM25;
    total.sumaPM25 += origen.sumaPM25;
    total.cantidadPM10 += origen.cantidadPM10;
    total.sumaPM10 += origen.sumaPM10;
}

void procesarReportePendiente() {
    if (!solicitudReportePendiente) {
        return;
    }
    solicitudReportePendiente = false;

    time_t desde = reporteDesde;
    time_t hasta = reporteHasta;
    uint32_t intervalo = reporteIntervaloSegundos;
    if (!sdIniciada || caracteristicaReporteBLE == nullptr) {
        notificarReporte("ERR|SD");
        return;
    }
    if ((intervalo != INTERVALO_15M && intervalo != INTERVALO_1H) ||
        hasta <= desde) {
        notificarReporte("ERR|RANGO");
        return;
    }

    mostrarEstadoConexion("REPORTE", "PREPARANDO", "Leyendo indice rapido",
                          GC9A01A_BLUE);
    const char* ruta = intervalo == INTERVALO_15M
            ? ARCHIVO_RESUMEN_15M : ARCHIVO_RESUMEN_1H;
    int cantidadPuntos = leerResumenReciente(ruta, intervalo, desde, hasta);
    if (cantidadPuntos < 0) {
        notificarReporte("ERR|ARCHIVO");
        return;
    }
    ordenarRegistrosReporte(cantidadPuntos);

    RegistroResumen total;
    reiniciarRegistroResumen(total, intervalo, 0);
    time_t primeraMuestra = 0;
    time_t ultimaMuestra = 0;
    for (int i = 0; i < cantidadPuntos; i++) {
        fusionarRegistro(total, registrosReporte[i]);
        if (primeraMuestra == 0 ||
            registrosReporte[i].inicio < (uint32_t)primeraMuestra) {
            primeraMuestra = registrosReporte[i].inicio;
        }
        time_t fin = (time_t)registrosReporte[i].inicio + intervalo;
        if (fin > ultimaMuestra) {
            ultimaMuestra = fin;
        }
    }

    notificarReporte("B");
    char mensaje[150];
    snprintf(mensaje, sizeof(mensaje), "S|%lu|%lu|%lu|%lu|%lld|%lld",
             (unsigned long)total.cantidad, (unsigned long)total.buenas,
             (unsigned long)total.moderadas,
             (unsigned long)total.deficientes,
             (long long)primeraMuestra, (long long)ultimaMuestra);
    notificarReporte(mensaje);

    if (total.cantidad > 0) {
        snprintf(mensaje, sizeof(mensaje),
                 "M|%.1f|%.0f|%.0f|%.1f|%.1f|%.1f|%.1f|%.1f|%.1f",
                 total.sumaCo2 / total.cantidad,
                 total.minimoCo2, total.maximoCo2,
                 total.sumaTemperatura / total.cantidad,
                 total.minimaTemperatura, total.maximaTemperatura,
                 total.sumaHumedad / total.cantidad,
                 total.minimaHumedad, total.maximaHumedad);
        notificarReporte(mensaje);
        snprintf(mensaje, sizeof(mensaje), "Q|%.1f|%.0f|%.0f|%.1f|%.0f|%.0f",
                 total.cantidadPM25
                    ? total.sumaPM25 / total.cantidadPM25 : -1.0,
                 total.cantidadPM25 ? total.minimoPM25 : 0,
                 total.cantidadPM25 ? total.maximoPM25 : 0,
                 total.cantidadPM10
                    ? total.sumaPM10 / total.cantidadPM10 : -1.0,
                 total.cantidadPM10 ? total.minimoPM10 : 0,
                 total.cantidadPM10 ? total.maximoPM10 : 0);
        notificarReporte(mensaje);
    }

    char paquete[178] = {};
    for (int i = 0; i < cantidadPuntos && clienteBLEConectado; i++) {
        RegistroResumen& punto = registrosReporte[i];
        time_t instantePunto = (time_t)punto.inicio + intervalo / 2;
        if (instantePunto >= hasta) {
            instantePunto = hasta - 1;
        }
        float promedioPM25 = punto.cantidadPM25
                ? punto.sumaPM25 / punto.cantidadPM25 : -1;
        float promedioPM10 = punto.cantidadPM10
                ? punto.sumaPM10 / punto.cantidadPM10 : -1;
        char linea[70];
        snprintf(linea, sizeof(linea), "P|%lld|%.1f|%.1f|%.1f|%.1f|%.1f",
                 (long long)instantePunto,
                 punto.sumaCo2 / punto.cantidad,
                 punto.sumaTemperatura / punto.cantidad,
                 punto.sumaHumedad / punto.cantidad,
                 promedioPM25, promedioPM10);
        size_t usado = strlen(paquete);
        size_t largoLinea = strlen(linea);
        if (usado > 0 && usado + 1 + largoLinea >= sizeof(paquete)) {
            notificarReporte(paquete);
            paquete[0] = '\0';
            usado = 0;
        }
        if (usado > 0) {
            strncat(paquete, "\n", sizeof(paquete) - strlen(paquete) - 1);
        }
        strncat(paquete, linea, sizeof(paquete) - strlen(paquete) - 1);
    }
    if (paquete[0] != '\0') {
        notificarReporte(paquete);
    }

    notificarReporte("E");
    Serial.print("Reporte BLE rapido enviado. Bloques: ");
    Serial.print(cantidadPuntos);
    Serial.print(" | Muestras resumidas: ");
    Serial.println(total.cantidad);
    mostrarEstadoConexion("REPORTE", "ENVIADO", "Datos listos en la app", GC9A01A_GREEN);
}
#endif

void mostrarPantallaInicio() {
    pantalla.fillScreen(GC9A01A_BLACK);
    pantalla.setTextWrap(false);
    pantalla.setTextColor(GC9A01A_CYAN);
    pantalla.setTextSize(2);
    pantalla.setCursor(48, 62);
    pantalla.println("CALIDAD");
    pantalla.setCursor(70, 86);
    pantalla.println("DEL AIRE");
    pantalla.drawCircle(120, 120, 116, GC9A01A_BLUE);
    pantalla.setTextColor(GC9A01A_WHITE);
    pantalla.setTextSize(1);
    pantalla.setCursor(55, 155);
    pantalla.println("Iniciando sensores...");
}

void mostrarEstadoSD(bool disponible) {
    pantalla.fillScreen(GC9A01A_BLACK);
    pantalla.drawCircle(120, 120, 116,
                        disponible ? GC9A01A_GREEN : GC9A01A_RED);
    pantalla.setTextWrap(false);
    pantalla.setTextSize(2);
    pantalla.setTextColor(disponible ? GC9A01A_GREEN : GC9A01A_RED);
    pantalla.setCursor(62, 78);
    pantalla.println("MicroSD");
    pantalla.setTextColor(GC9A01A_WHITE);
    pantalla.setTextSize(2);
    pantalla.setCursor(disponible ? 60 : 70, 112);
    pantalla.println(disponible ? "INICIADA" : "ERROR");
    pantalla.setTextSize(1);
    pantalla.setCursor(disponible ? 49 : 39, 150);
    pantalla.println(disponible ? "Guardando datos CSV" : "Revise tarjeta y cableado");
}

void mostrarEstadoConexion(const char* titulo, const char* estado,
                           const char* detalle, uint16_t color) {
    int16_t x1, y1;
    uint16_t ancho, alto;

    pantalla.fillScreen(GC9A01A_BLACK);
    pantalla.drawCircle(120, 120, 116, color);
    pantalla.setTextWrap(false);
    pantalla.setTextSize(2);

    pantalla.setTextColor(GC9A01A_WHITE);
    pantalla.getTextBounds(titulo, 0, 0, &x1, &y1, &ancho, &alto);
    pantalla.setCursor((240 - ancho) / 2, 65);
    pantalla.println(titulo);

    pantalla.setTextColor(color);
    pantalla.getTextBounds(estado, 0, 0, &x1, &y1, &ancho, &alto);
    pantalla.setCursor((240 - ancho) / 2, 105);
    pantalla.println(estado);

    pantalla.setTextColor(GC9A01A_WHITE);
    pantalla.setTextSize(1);
    pantalla.getTextBounds(detalle, 0, 0, &x1, &y1, &ancho, &alto);
    pantalla.setCursor((240 - ancho) / 2, 154);
    pantalla.println(detalle);
}

void mostrarMedicion(uint16_t co2, float temperatura, float humedad) {
    pantalla.fillScreen(GC9A01A_BLACK);
    pantalla.drawCircle(120, 120, 116, GC9A01A_BLUE);

    pantalla.setTextColor(GC9A01A_WHITE);
    pantalla.setTextSize(2);
    pantalla.setCursor(76, 18);
    pantalla.println("CO2");
    pantalla.setTextColor(GC9A01A_GREEN);
    pantalla.setTextSize(3);
    pantalla.setCursor(51, 43);
    pantalla.print(co2);
    pantalla.setTextSize(2);
    pantalla.println(" ppm");

    pantalla.drawFastHLine(36, 86, 168, GC9A01A_DARKGREY);
    pantalla.setTextColor(GC9A01A_YELLOW);
    pantalla.setTextSize(2);
    pantalla.setCursor(34, 98);
    pantalla.print("Temp: ");
    pantalla.print(temperatura, 1);
    pantalla.println(" C");
    pantalla.setCursor(34, 123);
    pantalla.setTextColor(GC9A01A_CYAN);
    pantalla.print("Hum:  ");
    pantalla.print(humedad, 1);
    pantalla.println(" %");

    bool pmValida = lecturaPMDisponible();

    pantalla.setTextColor(COLOR_PM25);
    pantalla.setCursor(22, 148);
    pantalla.print("PM2.5: ");
    if (pmValida) {
        pantalla.print(ultimaPM.pm25);
    } else {
        pantalla.print("--");
    }
    pantalla.println(" ug/m3");

    pantalla.setTextColor(GC9A01A_MAGENTA);
    pantalla.setCursor(22, 172);
    pantalla.print("PM10:  ");
    if (pmValida) {
        pantalla.print(ultimaPM.pm10);
    } else {
        pantalla.print("--");
    }
    pantalla.println(" ug/m3");

    pantalla.setTextSize(2);
    int16_t x1, y1;
    uint16_t ancho, alto;
    const char* estado = clasificarVentilacion(co2);
    pantalla.getTextBounds(estado, 0, 0, &x1, &y1, &ancho, &alto);
    pantalla.setTextColor(colorClasificacion(co2));
    pantalla.setCursor((240 - ancho) / 2, 202);
    pantalla.println(estado);
}

void mostrarError(const char* funcion, int16_t codigo) {
    Serial.print("Error en ");
    Serial.print(funcion);
    Serial.print(": ");

    errorToString(codigo, errorMessage, sizeof(errorMessage));
    Serial.println(errorMessage);
}

bool recuperarMedicionPeriodicaSCD() {
    // El diagnostico no vuelve a OK hasta recibir una medicion valida nueva.
    falloSCDActivo = true;
    reintentosMedicionSCD++;
    char detalle[28];
    snprintf(detalle, sizeof(detalle), "Reintento automatico %u",
             reintentosMedicionSCD);
    mostrarEstadoConexion("SENSOR CO2", "RECUPERANDO", detalle,
                          GC9A01A_YELLOW);
    Serial.print("SCD41 sin datos. Reiniciando medicion periodica, intento ");
    Serial.println(reintentosMedicionSCD);

    // La propia biblioteca aplica aquí los 500 ms exigidos por el sensor.
    int16_t errorDetencion = sensor.stopPeriodicMeasurement();
    if (errorDetencion != NO_ERROR) {
        mostrarError("stopPeriodicMeasurement recovery", errorDetencion);
    }

    error = sensor.startPeriodicMeasurement();
    if (error != NO_ERROR) {
        mostrarError("startPeriodicMeasurement recovery", error);
        mostrarEstadoConexion("SENSOR CO2", "ERROR",
                              "No pudo reiniciar medicion", GC9A01A_RED);
        inicioEsperaSCDMs = millis();
        return false;
    }

    inicioEsperaSCDMs = millis();
    if (primeraMedicionSCDRecibida) {
        ultimaMedicionSCDMs = inicioEsperaSCDMs;
    }
    ultimoAvisoEsperaSCDMs = 0;
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(150);

    Serial.println();
    Serial.println("==============================");
    Serial.println("Prueba SCD41 con ESP32-S3");
    Serial.println("SDA: GPIO 8");
    Serial.println("SCL: GPIO 9");
    Serial.println("PM sensor TX -> GPIO 17");
    Serial.println("PM sensor RX -> GPIO 18");
    Serial.println("==============================");

    iniciarBluetooth();

    SPI.begin(TFT_SCLK, SD_MISO, TFT_MOSI, TFT_CS);
    pantalla.begin();
    pantalla.setRotation(0);

    // Se valida tanto el montaje como la apertura de escritura del archivo.
    if (SD.begin(SD_CS, SPI)) {
        bool archivoExistente = SD.exists(ARCHIVO_DATOS_30S);
        File archivo = SD.open(ARCHIVO_DATOS_30S, FILE_APPEND);
        if (archivo) {
            if (!archivoExistente) {
                archivo.println(
                    "fecha_hora,muestras,co2_promedio_ppm,co2_min_ppm,"
                    "co2_max_ppm,temperatura_promedio_c,humedad_promedio_pct,"
                    "pm25_muestras,pm25_promedio_ug_m3,pm25_min_ug_m3,"
                    "pm25_max_ug_m3,pm10_muestras,pm10_promedio_ug_m3,"
                    "pm10_min_ug_m3,pm10_max_ug_m3,vent_buenas,"
                    "vent_moderadas,vent_deficientes"
                );
            }
            archivo.flush();
            bool preparacionCorrecta = archivo.getWriteError() == 0;
            archivo.close();
            if (preparacionCorrecta) {
                sdIniciada = true;
                falloEscrituraSDActivo = false;
                Serial.println("MicroSD iniciada. Promedios cada 30 s en /calidad_aire_30s.csv");
            } else {
                falloEscrituraSDActivo = true;
                Serial.println("Aviso: fallo la preparacion de escritura en la microSD.");
            }
        } else {
            falloEscrituraSDActivo = true;
            Serial.println("Aviso: la microSD monto, pero no permite escribir.");
        }
    } else {
        falloEscrituraSDActivo = true;
        Serial.println("Aviso: no se pudo iniciar la microSD.");
    }
    mostrarEstadoSD(sdIniciada);

    // La hora llega desde Android por BLE. Se elimina la espera de Wi-Fi y
    // HTTPS, que podia bloquear el arranque durante decenas de segundos.
    setenv("TZ", ZONA_HORARIA_CHILE, 1);
    tzset();
    Serial.println("Hora pendiente: se recibira desde la app por Bluetooth.");
    mostrarEstadoConexion("SENSORES", "INICIANDO",
                          "Preparando UART e I2C", GC9A01A_BLUE);

    sensorPM.begin(PM_BAUD, SERIAL_8N1, PM_RX_PIN, PM_TX_PIN);
    sensorPMIniciado = true;
    inicioSensorPMMs = millis();
    Serial.println("Sensor PM por UART iniciado.");

    // Bus I2C de la ESP32-S3
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000);
    // Evita que un bus desconectado deje la inicialización esperando durante
    // varios segundos. Los errores se informan en la TFT y BLE sigue activo.
    Wire.setTimeOut(80);

    // Dirección I2C del SCD41: 0x62
    sensor.begin(Wire, SCD41_I2C_ADDR_62);
    mostrarEstadoConexion("SENSOR CO2", "INICIANDO",
                          "Reiniciando SCD41", GC9A01A_BLUE);

    /*
     * Despertar el sensor.
     * wakeUp puede devolver NACK en el SCD41, por lo que
     * no detenemos el programa aunque informe error.
     */
    mostrarEstadoConexion("SENSOR CO2", "INICIANDO",
                          "Despertando SCD41", GC9A01A_BLUE);
    sensor.wakeUp();

    /*
     * Detener una posible medición anterior.
     */
    mostrarEstadoConexion("SENSOR CO2", "INICIANDO",
                          "Preparando medicion", GC9A01A_BLUE);
    error = sensor.stopPeriodicMeasurement();

    if (error != NO_ERROR) {
        Serial.println(
            "Aviso: no se pudo detener una medicion anterior."
        );
    }

    // La biblioteca ya espera internamente los 500 ms requeridos por el
    // fabricante dentro de stopPeriodicMeasurement(); no se duplica aquí.
    mostrarEstadoConexion("SENSOR CO2", "INICIANDO",
                          "Aplicando reinicio", GC9A01A_BLUE);

    /*
     * Reinicializar el sensor.
     */
    error = sensor.reinit();

    if (error != NO_ERROR) {
        mostrarError("reinit", error);
        mostrarEstadoConexion("SENSOR CO2", "ERROR",
                              "Revise I2C GPIO 8/9", GC9A01A_RED);
        return;
    }

    /*
     * Leer el número de serie para confirmar la comunicación.
     */
    uint64_t numeroSerie = 0;

    error = sensor.getSerialNumber(numeroSerie);

    if (error != NO_ERROR) {
        mostrarError("getSerialNumber", error);
        mostrarEstadoConexion("SENSOR CO2", "ERROR",
                              "No responde por I2C", GC9A01A_RED);
        return;
    }

    Serial.print("Numero de serie: 0x");
    Serial.print((uint32_t)(numeroSerie >> 32), HEX);
    Serial.print((uint32_t)(numeroSerie & 0xFFFFFFFF), HEX);
    Serial.println();

    /*
     * Iniciar mediciones periódicas.
     * El sensor genera una medición nueva aproximadamente
     * cada cinco segundos.
     */
    error = sensor.startPeriodicMeasurement();

    if (error != NO_ERROR) {
        mostrarError("startPeriodicMeasurement", error);
        mostrarEstadoConexion("SENSOR CO2", "ERROR",
                              "No inicio mediciones", GC9A01A_RED);
        return;
    }

    sensorIniciado = true;
    falloSCDActivo = false;
    primeraMedicionSCDRecibida = false;
    inicioEsperaSCDMs = millis();
    ultimaMedicionSCDMs = 0;
    ultimaLecturaSCDValidaMs = 0;
    ultimoAvisoEsperaSCDMs = 0;
    reintentosMedicionSCD = 0;
    erroresI2CSCDConsecutivos = 0;

    Serial.println("SCD41 iniciado correctamente.");
    Serial.println("Esperando la primera medicion (normal: 5 a 7 s)...");
    mostrarEstadoConexion("SENSOR CO2", "LISTO",
                          "Primer dato en 5 a 7 s", GC9A01A_GREEN);
}

void loop() {
    actualizarLecturaPM();
    // Este latido va antes de cualquier retorno para que la app reciba el
    // diagnostico incluso si el SCD41 no pudo iniciar o esta recuperandose.
    publicarEstadoBluetooth();

    if (avisoHoraMovilPendiente) {
        avisoHoraMovilPendiente = false;
        mostrarEstadoConexion("HORA MOVIL", "LISTA",
                              "Sincronizada por Bluetooth", GC9A01A_GREEN);
    }

    if (!sensorIniciado) {
        delay(20);
        return;
    }

    // El loop sigue atendiendo UART y BLE continuamente. El SCD41 solo se
    // consulta cada 250 ms, sin detener todo el sistema durante cinco segundos.
    static unsigned long proximaConsultaSCD = 0;
    unsigned long ahoraMs = millis();
    if ((int32_t)(ahoraMs - proximaConsultaSCD) < 0) {
        delay(5);
        return;
    }
    proximaConsultaSCD = ahoraMs + 250;

    bool datosDisponibles = false;

    error = sensor.getDataReadyStatus(datosDisponibles);

    if (error != NO_ERROR) {
        mostrarError("getDataReadyStatus", error);
        erroresI2CSCDConsecutivos++;
        if (erroresI2CSCDConsecutivos >= 3) {
            falloSCDActivo = true;
            mostrarEstadoConexion("SENSOR CO2", "ERROR I2C",
                                  "Reintentando comunicacion", GC9A01A_RED);
        }
        if (erroresI2CSCDConsecutivos >= 5) {
            erroresI2CSCDConsecutivos = 0;
            recuperarMedicionPeriodicaSCD();
        }
        proximaConsultaSCD = millis() + 1000;
        return;
    }
    erroresI2CSCDConsecutivos = 0;

    if (!datosDisponibles) {
        unsigned long sinDatosMs = primeraMedicionSCDRecibida
                ? ahoraMs - ultimaMedicionSCDMs
                : ahoraMs - inicioEsperaSCDMs;

        // La TFT confirma que el programa sigue vivo mientras el SCD41 forma
        // su primera muestra. No se redibuja más de una vez por segundo.
        if (!primeraMedicionSCDRecibida &&
            ahoraMs - ultimoAvisoEsperaSCDMs >= 1000) {
            ultimoAvisoEsperaSCDMs = ahoraMs;
            char detalle[28];
            snprintf(detalle, sizeof(detalle), "Esperando %lu s",
                     sinDatosMs / 1000UL);
            mostrarEstadoConexion("SENSOR CO2", "LISTO", detalle,
                                  GC9A01A_GREEN);
            Serial.print("SCD41 preparando primera muestra: ");
            Serial.print(sinDatosMs / 1000UL);
            Serial.println(" s");
        }

        // Una primera muestra suele tardar unos cinco segundos. Doce segundos
        // permiten margen suficiente; después se recupera automáticamente.
        unsigned long limiteMs =
                primeraMedicionSCDRecibida ? 15000UL : 12000UL;
        if (sinDatosMs >= limiteMs) {
            recuperarMedicionPeriodicaSCD();
        }
        return;
    }

    uint16_t co2 = 0;
    float temperatura = 0.0;
    float humedad = 0.0;

    error = sensor.readMeasurement(
        co2,
        temperatura,
        humedad
    );

    if (error != NO_ERROR) {
        falloSCDActivo = true;
        mostrarError("readMeasurement", error);
        mostrarEstadoConexion("SENSOR CO2", "ERROR I2C",
                              "Fallo al leer medicion", GC9A01A_RED);
        proximaConsultaSCD = millis() + 1000;
        return;
    }

    if (co2 == 0) {
        Serial.println("Medicion de CO2 todavia no valida.");
        return;
    }

    primeraMedicionSCDRecibida = true;
    ultimaMedicionSCDMs = millis();
    ultimaLecturaSCDValidaMs = ultimaMedicionSCDMs;
    falloSCDActivo = false;
    reintentosMedicionSCD = 0;

    Serial.println("--------------------------------");

    Serial.print("CO2: ");
    Serial.print(co2);
    Serial.print(" ppm - Ventilacion ");
    Serial.println(clasificarVentilacion(co2));

    Serial.print("Temperatura: ");
    Serial.print(temperatura, 2);
    Serial.println(" °C");

    Serial.print("Humedad relativa: ");
    Serial.print(humedad, 2);
    Serial.println(" %RH");

    if (lecturaPMDisponible()) {
        Serial.print("PM2.5: ");
        Serial.print(ultimaPM.pm25);
        Serial.println(" ug/m3");

        Serial.print("PM10: ");
        Serial.print(ultimaPM.pm10);
        Serial.println(" ug/m3");
    } else {
        Serial.println("PM2.5/PM10: sin lectura valida aun.");
    }

    mostrarMedicion(co2, temperatura, humedad);
    registrarMedicionSD(co2, temperatura, humedad);
    enviarMedicionBluetooth(co2, temperatura, humedad);
}
