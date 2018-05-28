#include "stdafx.h"
#include "MqttLite.h"
#include "../main/Logger.h"
#include "../main/Helper.h"
#include <iostream>
#include "../main/localtime_r.h"
#include "../main/mainworker.h"
#include "../main/SQLHelper.h"
#include "../json/json.h"
#include "../main/WebServer.h"
#include "hardwaretypes.h"

#include "../notifications/NotificationHelper.h"

#define RETRY_DELAY 30

#define CLIENTID	"Domoticz"
#define TOPIC_OUT	"actuators/"
#define TOPIC_IN	"/sensors/#"
#define TOPIC_DEVICE "/device/#"
#define QOS         1

MQTT_Lite::MQTT_Lite(const int ID, const std::string &IPAddress, const unsigned short usIPPort, const std::string &Username, const std::string &Password, const std::string &CAfilename) :
m_szIPAddress(IPAddress),
m_UserName(Username),
m_Password(Password),
m_CAFilename(CAfilename)
{
	_log.Log(LOG_STATUS, "MQTT_Lite: creat");
	m_HwdID=ID;
	m_IsConnected = false;
	m_bDoReconnect = false;
	mosqpp::lib_init();

	m_stoprequested=false;
	m_usIPPort=usIPPort;
	//m_publish_topics = (_ePublishTopics)Topics;
}

MQTT_Lite::~MQTT_Lite(void)
{
	mosqpp::lib_cleanup();
}

bool MQTT_Lite::StartHardware()
{
	_log.Log(LOG_STATUS, "MQTT_Lite: start");
	StartHeartbeatThread();

	m_stoprequested=false;

	//force connect the next first time
	m_IsConnected=false;

	m_bIsStarted = true;

	//Start worker thread
	m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&MQTT_Lite::Do_Work, this)));
	return (m_thread!=NULL);
}

void MQTT_Lite::StopMQTT_Lite()
{
	disconnect();
	m_bIsStarted = false;
}

bool MQTT_Lite::StopHardware()
{
	StopHeartbeatThread();
	m_stoprequested=true;
	try {
		if (m_thread)
		{
			m_thread->join();
			m_thread.reset();
		}
	}
	catch (...)
	{
		//Don't throw from a Stop command
	}
	if (m_sConnection.connected())
		m_sConnection.disconnect();
	m_IsConnected = false;
	return true;
}

void MQTT_Lite::on_subscribe(int mid, int qos_count, const int *granted_qos)
{
	_log.Log(LOG_STATUS, "MQTT_Lite: Subscribed");
	m_IsConnected = true;
}

void MQTT_Lite::on_connect(int rc)
{
	/* rc=
	** 0 - success
	** 1 - connection refused(unacceptable protocol version)
	** 2 - connection refused(identifier rejected)
	** 3 - connection refused(broker unavailable)
	*/

	if (rc == 0){
		if (m_IsConnected) {
			_log.Log(LOG_STATUS, "MQTT_Lite: re-connected to: %s:%ld", m_szIPAddress.c_str(), m_usIPPort);
		} else {
			_log.Log(LOG_STATUS, "MQTT_Lite: connected to: %s:%ld", m_szIPAddress.c_str(), m_usIPPort);
			m_IsConnected = true;
			sOnConnected(this);
			m_sConnection = m_mainworker.sOnDeviceReceived.connect(boost::bind(&MQTT_Lite::SendDeviceInfo, this, _1, _2, _3, _4));
		}
		subscribe(NULL, TOPIC_IN);
		subscribe(NULL, TOPIC_DEVICE);
	}
	else {
		_log.Log(LOG_ERROR, "MQTT_Lite: Connection failed!, restarting (rc=%d)",rc);
		m_bDoReconnect = true;
	}
}

typedef enum
{
	TEMPERATURE=0,
	TEMPERATURE_MIN,
	TEMPERATURE_MAX,
	CURRENT,
	POWER,
	HUMIDITY,
	LIGHT,
	VOLTAGE,
	BATTERY,
	PRESSURE,
	SPEED,
	DIRECTION,
	ICON,
	SWITCH,
	POURCENT
}sensor_type_t;

std::vector<std::string> split(const std::string &s, char delim) {
		std::stringstream ss(s);
		std::string item;
		std::vector<std::string> elems;
		while (getline(ss, item, delim)) {
				elems.push_back(item);
		}
		return elems;
}

std::string join(const std::vector<std::string> &s, char delim) {
		std::string string;
		int l;
		for(l=0;l<s.size()-1;l++) {
				string+=s[l];
				string+=delim;
		}
		string+=s[l];
		return string;
}

void MQTT_Lite::on_message(const struct mosquitto_message *message)
{
#define HEADER_SIZE 6
	if(message==NULL)
		return;
	char * data = (char*)message->payload;
	std::string topic = message->topic;
	int length=MAX_LENGTH_STATION_NAME;
	if(message->payloadlen<HEADER_SIZE+1)//si on a pas aux minimum le header et un nom de 1 character
		return;
	if(message->payloadlen<(length+HEADER_SIZE))
		 length=message->payloadlen-HEADER_SIZE;
	std::string name(data+HEADER_SIZE,length);
	if((name.size()==0) || name[0]==0)
		return;
	sensor_type_t type=(sensor_type_t)data[1];
	bool update=0;


	_log.Log(LOG_NORM, "MQTT_Lite: Topic: %s, Device: %s",topic.c_str(), name.c_str());

	std::vector<std::vector<std::string> > result;

	//Get the raw device parameters
	while(name[name.size()-1]==0)
		name.erase(name.size()-1);

	if(type==POWER)
		name=name+"_"; //pour differentier le capteur de l'interrupter

	result = m_sql.safe_query("SELECT HardwareID, ID, Unit, Type, SubType, DeviceID, svalue, BatteryLevel, nvalue, LastUpdate FROM DeviceStatus WHERE (Name='%q')", name.c_str());

	if (result.empty())
	{
		_log.Log(LOG_ERROR, "MQTT_Lite: unknown idx received!");
		return;
	}
	for(int l=0;l<result.size();l++)
	{
		int HardwareID = atoi(result[l][0].c_str());
		//std::string idx = result[0][1];
		int idx= atoi(result[l][1].c_str());
		int unit = atoi(result[l][2].c_str());
		int devType = atoi(result[l][3].c_str());
		int subType = atoi(result[l][4].c_str());
		std::string stringDeviceID = result[l][5];
		std::string svalue =result[l][6];
		int batterylevel = atoi(result[l][7].c_str());
		bool bParseTrigger =true;
		int nvalue=atoi(result[l][8].c_str());
		std::string sLastUpdate=result[l][9];
		//"0.0;50;1;1010;1"

		if(devType==pTypeTEMP_HUM_BARO_VOLT)
		{
			update=1;
			std::vector<std::string> svaluesplit=split(svalue,';'); //split les valeurs temperature, humidit?, humidit? status, presion, presion status , tension
			svaluesplit.resize(6);

			int value =0;
			for(int l=0;l<4;l++)
			{
				value|=(unsigned char)data[2+l]<<((3-l)*8);
			}
			std::ostringstream ss;

			switch(type)
			{
				case TEMPERATURE:
					ss << value/100.f;
					svaluesplit[0]=ss.str();
					nvalue=value/100;
				break;
				case HUMIDITY:
					if(value>100*100)
						value=100*100;
					else if(value<0)
						value=0;

					ss << value/100.f;
					svaluesplit[1]=ss.str();
				break;
				case PRESSURE:
					ss << value/100.f;
					svaluesplit[3]=ss.str();
				break;
				case BATTERY:
					ss << value/1000.f;
					svaluesplit[5]=ss.str();
					batterylevel=value/(value/1800+1);
					batterylevel=(batterylevel-1200)/3.1;
					if(batterylevel>100)
						batterylevel=100;
					if(batterylevel<0)
						batterylevel=0;
				break;
			}

			svalue=join(svaluesplit,';');
		}

		if(devType==pTypeHUM)
		{

			update=1;

			int value =0;
			for(int l=0;l<4;l++)
			{
				value|=(unsigned char)data[2+l]<<((3-l)*8);
			}
			std::ostringstream ss;

			switch(type)
			{
				case HUMIDITY:
					if(value>100*100)
						value=100*100;
					else if(value<0)
						value=0;
					nvalue=value/100;

					ss << value/100.f;
					svalue=ss.str();
				break;
			}
		}

		if(devType==pTypeGeneral && subType==sTypeKwh)
		{
			update=1;
			std::vector<std::string> svaluesplit=split(svalue,';'); //split les valeurs
			svaluesplit.resize(2);

			int value =0;
			for(int l=0;l<4;l++)
			{
				value|=(unsigned char)data[2+l]<<((3-l)*8);
			}
			std::ostringstream ss;

			switch(type)
			{
				case POWER:
					ss << value/1000.f;
					svaluesplit[0]=ss.str();
					nvalue=value/1000;

					float value_kwh=atof(svaluesplit[1].c_str());
					struct tm ntime;
					time_t lutime;
					ParseSQLdatetime(lutime, ntime, sLastUpdate); //not perfect why he don't count time betwen her and when update in database
					std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
					float dt;
					if(old_time.find(idx) != old_time.end())
					{
						std::chrono::duration<double> delta = std::chrono::duration_cast<std::chrono::duration<double>>(now-old_time[idx]);
						dt=delta.count();
						if(dt>90*60) //if now info duiring more of 1h30 do not count
						{
							dt=0;
						}
					}
					else
						dt=0;
					old_time[idx]=now;
					value_kwh+=value/1000.f/60/60*dt;
					//printf("dt =%f\n",dt);
					std::ostringstream ss2;
					ss2.precision(7);
					ss2 << value_kwh;
					svaluesplit[1]=ss2.str();

				break;
				/*case POWER:
					ss << value/100.f;
					svaluesplit[2]=ss.str();
				break;*/
			}
			svalue=join(svaluesplit,';');
		}

		if(devType==pTypeGeneralSwitch && subType==sSwitchGeneralSwitch)
		{
			update=1;
			int value=0;
			for(int l=0;l<4;l++)
			{
				value|=(unsigned char)data[2+l]<<((3-l)*8);
			}
			std::ostringstream ss;
			switch(type)
			{
				case SWITCH:
					if(value) //test sans
					{
						svalue="100";
						nvalue=1;
					}
					else
					{
						svalue="0";
						nvalue=0;
					}
				break;
				case POURCENT:
					nvalue=value;
					ss << value;
					svalue=ss.str();

				break;
				case BATTERY:
					update=1;
					batterylevel=value/(value/1800+1);
					batterylevel=(batterylevel-1200)/3.1;
					if(batterylevel>100)
						batterylevel=100;
					if(batterylevel<0)
						batterylevel=0;
				break;
			}
		}

		if(devType==pTypeGeneral && subType==sTypeCustom)
		{
			update=1;
			int value=0;
			for(int l=0;l<4;l++)
			{
				value|=(unsigned char)data[2+l]<<((3-l)*8);
			}
			std::ostringstream ss;
			switch(type)
			{
				case BATTERY:
					update=1;
					batterylevel=value/(value/1800+1);
					batterylevel=(batterylevel-1200)/3.1;
					if(batterylevel>100)
						batterylevel=100;
					if(batterylevel<0)
						batterylevel=0;
				break;
				default:
					nvalue=value;
					ss << value;
					svalue=ss.str();
				break;
			}
		}

		int signallevel = 12;

		if(update)
		{
			_log.Log(LOG_TRACE, "batterylevel=%d\n",batterylevel);
			if (!m_mainworker.UpdateDevice(HardwareID, stringDeviceID, unit, devType, subType, nvalue, svalue, signallevel, batterylevel, bParseTrigger))
			{
				_log.Log(LOG_ERROR, "MQTT_Lite: Problem updating sensor (check idx, hardware enabled)");
			}
		}
	}
	return;
/*mqttinvaliddata:
	_log.Log(LOG_ERROR, "MQTT_Lite: Invalid data received!");*/
}

void MQTT_Lite::on_disconnect(int rc)
{
	if (rc != 0)
	{
		if (!m_stoprequested)
		{
			if (rc == 5)
			{
				_log.Log(LOG_ERROR, "MQTT_Lite: disconnected, Invalid Username/Password (rc=%d)", rc);
			}
			else
			{
				_log.Log(LOG_ERROR, "MQTT_Lite: disconnected, restarting (rc=%d)", rc);
			}
			m_bDoReconnect = true;
		}
	}
}


bool MQTT_Lite::ConnectInt()
{
	StopMQTT_Lite();
	return ConnectIntEx();
}

bool MQTT_Lite::ConnectIntEx()
{
	m_bDoReconnect = false;
	_log.Log(LOG_STATUS, "MQTT_Lite: Connecting to %s:%d", m_szIPAddress.c_str(), m_usIPPort);

	int rc;
	int keepalive = 60;

	if (!m_CAFilename.empty()){
		rc = tls_set(m_CAFilename.c_str());

		if ( rc != MOSQ_ERR_SUCCESS)
		{
			_log.Log(LOG_ERROR, "MQTT_Lite: Failed enabling TLS mode, return code: %d (CA certificate: '%s')", rc, m_CAFilename.c_str());
			return false;
		} else {
			_log.Log(LOG_STATUS, "MQTT_Lite: enabled TLS mode");
		}
	}
	rc = username_pw_set((!m_UserName.empty()) ? m_UserName.c_str() : NULL, (!m_Password.empty()) ? m_Password.c_str() : NULL);

	rc = connect(m_szIPAddress.c_str(), m_usIPPort, keepalive);
	if ( rc != MOSQ_ERR_SUCCESS)
	{
		_log.Log(LOG_ERROR, "MQTT_Lite: Failed to start, return code: %d (Check IP/Port)", rc);
		m_bDoReconnect = true;
		return false;
	}
	return true;
}

void MQTT_Lite::Do_Work()
{
	bool bFirstTime=true;
	int msec_counter = 0;
	int sec_counter = 0;

	_log.Log(LOG_STATUS,"MQTT_Lite: Worker start");

	while (!m_stoprequested)
	{
		sleep_milliseconds(100);
		if (!bFirstTime)
		{
			int rc = loop();
			if (rc) {
				if (rc != MOSQ_ERR_NO_CONN)
				{
					if (!m_stoprequested)
					{
						if (!m_bDoReconnect)
						{
							reconnect();
						}
					}
				}
			}
		}

		msec_counter++;
		if (msec_counter == 10)
		{
			msec_counter = 0;

			sec_counter++;

			if (sec_counter % 12 == 0) {
				m_LastHeartbeat=mytime(NULL);
			}

			if (bFirstTime)
			{
				bFirstTime = false;
				ConnectInt();
			}
			else
			{
				if (sec_counter % 30 == 0)
				{
					if (m_bDoReconnect)
						ConnectIntEx();
				}
			}
		}
	}
	_log.Log(LOG_STATUS,"MQTT_Lite: Worker stopped...");
}

void MQTT_Lite::SendMessage(const std::string &Topic, const std::string &Message)
{
	try {
		if (!m_IsConnected)
		{
			_log.Log(LOG_STATUS, "MQTT_Lite: Not Connected, failed to send message: %s", Message.c_str());
			return;
		}
		publish(NULL, Topic.c_str(), Message.size(), Message.c_str(),1/*qos*/ ,true/*retain*/);
	}
	catch (...)
	{
		_log.Log(LOG_ERROR, "MQTT_Lite: Failed to send message: %s", Message.c_str());
	}
}

void MQTT_Lite::SendMessage(const std::string &Topic, const char* Message, const int length)
{
	try {
		if (!m_IsConnected)
		{
			_log.Log(LOG_STATUS, "MQTT_Lite: Not Connected, failed to send binary message");
			return;
		}
		publish(NULL, Topic.c_str(), length, Message,1/*qos*/ ,true/*retain*/);
	}
	catch (...)
	{
		_log.Log(LOG_ERROR, "MQTT_Lite: Failed to send binary message");
	}
}

void MQTT_Lite::WriteInt(const std::string &sendStr)
{
	boost::lock_guard<boost::mutex> l(m_mqtt_mutex);
	if (sendStr.size() < 2)
		return;
	//string the return and the end
	std::string sMessage = std::string(sendStr.begin(), sendStr.begin() + sendStr.size()-1);
	SendMessage("MyMQTT_Lite", sMessage);
}

/*void MQTT_Lite::ProcessMySensorsMessage(const std::string &MySensorsMessage)
{
	m_bufferpos = MySensorsMessage.size();
	memcpy(&m_buffer, MySensorsMessage.c_str(), m_bufferpos);
	m_buffer[m_bufferpos] = 0;
	ParseLine();
}*/

bool MQTT_Lite::WriteToHardware(const char *pdata, const unsigned char length)
{
	/*if(length == sizeof(_tLimitlessLights)) //use hue for stor temperatur of color
	{
		_tLimitlessLights* lcmd=(_tLimitlessLights*)pdata;

		std::vector<std::vector<std::string> > result;

		std::stringstream sstream;
		sstream << std::hex << lcmd->id;
		std::string stringDeviceID = sstream.str();
		stringDeviceID=std::string( 8-stringDeviceID.length(), '0')+stringDeviceID;

		//result = m_sql.safe_query("SELECT HardwareID, ID, Unit, Type, SubType, svalue, BatteryLevel, nvalue FROM DeviceStatus WHERE (ID='%q')", lcmd->id);
		result = m_sql.safe_query("SELECT HardwareID, ID, Unit, Type, SubType, svalue, BatteryLevel, nvalue FROM DeviceStatus WHERE (DeviceID='%q') and (Unit='%d')", stringDeviceID.c_str(),lcmd->dunit);
		int HardwareID = atoi(result[0][0].c_str());
		//std::string idx = result[0][1];
		int unit = atoi(result[0][2].c_str());
		int devType = atoi(result[0][3].c_str());
		int subType = atoi(result[0][4].c_str());
		std::string svalue =result[0][5];
		int batterylevel = atoi(result[0][6].c_str());
		int nvalue = atoi(result[0][7].c_str());
		bool bParseTrigger =true;
		uint8_t hue;

		std::vector<std::string> svaluesplit=split(svalue,';');
		svaluesplit.resize(2);

		if(lcmd->command==Limitless_SetRGBColour)
		{
			hue=lcmd->value;
		}
		if(lcmd->command==Limitless_SetColorToWhite)
		{
			hue=180;
		}

		std::ostringstream ss;
		ss << (int)hue;
		svaluesplit[1]=ss.str();
		svalue=join(svaluesplit,';');

		int signallevel = 12;
		if (!m_mainworker.UpdateDevice(HardwareID, stringDeviceID, unit, devType, subType, nvalue, svalue, signallevel, batterylevel, bParseTrigger))
		{
			_log.Log(LOG_ERROR, "MQTT_Lite: Problem updating sensor (check idx, hardware enabled)");
		}
	}*/
	return true;
}

void MQTT_Lite::SendDeviceInfo(const int m_HwdID, const unsigned long long DeviceRowIdx, const std::string &DeviceName, const unsigned char *pRXCommand)
{
	boost::lock_guard<boost::mutex> l(m_mqtt_mutex);
	if (!m_IsConnected)
		return;
	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT DeviceID, Unit, Name, [Type], SubType, nValue, sValue, SwitchType, SignalLevel, BatteryLevel, Options FROM DeviceStatus WHERE (HardwareID==%d) AND (ID==%llu)", m_HwdID, DeviceRowIdx);
	if (result.size() > 0)
	{
		std::vector<std::string> sd = result[0];
		std::string devID = sd[0];
		int unit = atoi(sd[1].c_str());
		std::string name = sd[2];
		int devType = atoi(sd[3].c_str());
		int subType = atoi(sd[4].c_str());
		int nvalue = atoi(sd[5].c_str());
		std::string svalue = sd[6];
		_eSwitchType switchType = (_eSwitchType)atoi(sd[7].c_str());
		int RSSI = atoi(sd[8].c_str());
		int BatteryLevel = atoi(sd[9].c_str());
		std::map<std::string, std::string> options = m_sql.BuildDeviceOptions(sd[10]);
		char message[4]="Sxx";
		//give all svalues separate

		if(devType==pTypeGeneralSwitch || devType==pTypeLimitlessLights)
		{
			switch(subType)
			{
				case sSwitch2Color:
				{
					std::vector<std::string> svaluesplit=split(svalue,';'); //split les valeurs puissance/couleur(temperature)
					svaluesplit.resize(2);
					/*if(nvalue==0)
					{
						svaluesplit[0]="0";
						svaluesplit[1]="0";
					}
					if(nvalue==1)
					{
						svaluesplit[0]="100";
						svaluesplit[1]="100";
					}*/
					int value=atoi(svaluesplit[0].c_str());
					int hue=atoi(svaluesplit[1].c_str());
					message[1]=value*hue/100;
					message[2]=value*(100-hue)/100;
					break;
			}

				default:
					if(nvalue==0)
						svalue="0";
					if(nvalue==1)
						svalue="100";
					std::vector<std::string> strarray;
					message[1]=atoi(svalue.c_str());
			}
			std::string topic(TOPIC_OUT);
			topic+=name;
			SendMessage(topic, message, sizeof(message));
		}

		time_t now = time(0);
		struct tm ltime;
		localtime_r(&now, &ltime);
		char szLastUpdate[40];
		sprintf(szLastUpdate, "%04d-%02d-%02d %02d:%02d:%02d", ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday, ltime.tm_hour, ltime.tm_min, ltime.tm_sec);
		m_sql.safe_query("UPDATE DeviceStatus SET nValue=%d, sValue='%q', LastUpdate='%q', SwitchType=%d WHERE (HardwareID == %d) AND (DeviceID == '%q')",
		nvalue, svalue.c_str(), szLastUpdate, switchType, m_HwdID, devID.c_str());

		/*if (!m_mainworker.UpdateDevice(m_HwdID, devID, unit, devType, subType, nvalue, svalue, RSSI, BatteryLevel, false))
		{
			_log.Log(LOG_ERROR, "MQTT_Lite: Problem updating sensor (check idx, hardware enabled)");
		}*/

		//(const _eSetType vType, const unsigned char Idx, const int SubUnit, const bool bOn, const double Level, const std::string &defaultname, const int BatLevel)

		//UpdateSwitch(devType,DeviceRowIdx,unit,nvalue,message[1],name,BatteryLevel);
	}
}

void MQTT_Lite::SendSceneInfo(const unsigned long long SceneIdx, const std::string &SceneName)
{
	std::vector<std::vector<std::string> > result, result2;
	result = m_sql.safe_query("SELECT ID, Name, Activators, Favorite, nValue, SceneType, LastUpdate, Protected, OnAction, OffAction, Description FROM Scenes WHERE (ID==%llu) ORDER BY [Order]", SceneIdx);
	if (result.empty())
		return;
	std::vector<std::vector<std::string> >::const_iterator itt;
	std::vector<std::string> sd = result[0];

	std::string sName = sd[1];
	std::string sLastUpdate = sd[6].c_str();

	unsigned char nValue = atoi(sd[4].c_str());
	unsigned char scenetype = atoi(sd[5].c_str());
	int iProtected = atoi(sd[7].c_str());

	//std::string onaction = base64_encode((const unsigned char*)sd[8].c_str(), sd[8].size());
	//std::string offaction = base64_encode((const unsigned char*)sd[9].c_str(), sd[9].size());

	Json::Value root;

	root["idx"] = sd[0];
	root["Name"] = sName;
	//root["Description"] = sd[10];
	//root["Favorite"] = atoi(sd[3].c_str());
	//root["Protected"] = (iProtected != 0);
	//root["OnAction"] = onaction;
	//root["OffAction"] = offaction;

	if (scenetype == 0)
	{
		root["Type"] = "Scene";
	}
	else
	{
		root["Type"] = "Group";
	}

	root["LastUpdate"] = sLastUpdate;

	if (nValue == 0)
		root["Status"] = "Off";
	else if (nValue == 1)
		root["Status"] = "On";
	else
		root["Status"] = "Mixed";
	root["Timers"] = (m_sql.HasSceneTimers(sd[0]) == true) ? "true" : "false";
	unsigned long long camIDX = m_mainworker.m_cameras.IsDevSceneInCamera(1, sd[0]);
	//root["UsedByCamera"] = (camIDX != 0) ? true : false;
	if (camIDX != 0) {
		std::stringstream scidx;
		scidx << camIDX;
		//root["CameraIdx"] = scidx.str();
	}
	std::string message = root.toStyledString();
	if (m_publish_topics & PT_out)
	{
		SendMessage(TOPIC_OUT, message);
	}
}

namespace http {
	namespace server {
		void CWebServer::RType_CreateMQTTsensor(WebEmSession & session, const request& req, Json::Value &root)
		{
			if (session.rights != 2)
			{
				//No admin user, and not allowed to be here
				return;
			}

			std::string idx = request::findValue(&req, "idx");
			std::string ssensorname = request::findValue(&req, "sensorname");
			std::string ssensortype = request::findValue(&req, "sensortype");
			std::string ssparameter = request::findValue(&req, "parameter");
			if ((idx == "") || (ssensortype.empty()) || (ssensorname.empty()))
				return;

			bool bCreated = false;
			int iSensorType = atoi(ssensortype.c_str());

			int HwdID = atoi(idx.c_str());

			//Make a unique number for ID
			std::vector<std::vector<std::string> > result;
			result = m_sql.safe_query("SELECT MAX(ID) FROM DeviceStatus");

			unsigned long nid = 0; //could be the first device ever

			if (result.size() > 0)
			{
				nid = atol(result[0][0].c_str());
			}
			nid += 82000;
			char ID[40];
			sprintf(ID, "%lu", nid);

			std::string devname;

			bool bPrevAcceptNewHardware = m_sql.m_bAcceptNewHardware;
			m_sql.m_bAcceptNewHardware = true;
			unsigned long long DeviceRowIdx = -1;
			std::string rID = std::string(ID);
			padLeft(rID, 8, '0');
			switch (iSensorType)
			{
			case 0:
				//Meteo
				{
					DeviceRowIdx=m_sql.UpdateValue(HwdID, rID.c_str(), 1, pTypeTEMP_HUM_BARO_VOLT, sTypeTHB1, 12, 255, 0, "0.0;0;1;0;1;0", devname);
					m_sql.safe_query("UPDATE DeviceStatus SET Options='%q', Name='%q' , Used=1 WHERE (ID==%llu)", ssensorname.c_str(),ssensorname.c_str(), DeviceRowIdx);
					bCreated = true;
					m_mainworker.m_eventsystem.GetCurrentStates();
				}
				break;
			case 1:
				//Power
				{
					DeviceRowIdx=m_sql.UpdateValue(HwdID, rID.c_str(), 1, pTypeGeneral, sTypeKwh, 12, 255, 0, "0.0;0.0", devname);
					m_sql.safe_query("UPDATE DeviceStatus SET Options='%q', Name='%q_' , Used=1 , SwitchType=%d WHERE (ID==%llu)", ssensorname.c_str(),ssensorname.c_str(),0, DeviceRowIdx);
					bCreated = true;
					m_mainworker.m_eventsystem.GetCurrentStates();
				}
				break;
			case 2:
				//Switch out
				{
				DeviceRowIdx=m_sql.UpdateValue(HwdID, rID.c_str() , 0, pTypeGeneralSwitch, sSwitchGeneralSwitch, 12, 255, 0, "100", devname);
				m_sql.safe_query("UPDATE DeviceStatus SET Options='%q', Name='%q' , Used=1 , SwitchType=%d WHERE (ID==%llu)", ssensorname.c_str(), ssensorname.c_str(), STYPE_Dimmer, DeviceRowIdx);
				bCreated = true;
				m_mainworker.m_eventsystem.GetCurrentStates();
				}
				break;
			case 3:
				//Switch int
				{
				DeviceRowIdx=m_sql.UpdateValue(HwdID, rID.c_str() , atoi(ssparameter.c_str()), pTypeGeneralSwitch, sSwitchGeneralSwitch, 12, 255, 0, "Off", devname);
				m_sql.safe_query("UPDATE DeviceStatus SET Options='%q', Name='%q' , Used=1 , SwitchType=%d WHERE (ID==%llu)", ssensorname.c_str(), ssensorname.c_str(), STYPE_OnOff, DeviceRowIdx);
				bCreated = true;
				m_mainworker.m_eventsystem.GetCurrentStates();
				}
				break;
			case 4:
				//pwm
				{
				DeviceRowIdx=m_sql.UpdateValue(HwdID, rID.c_str() , atoi(ssparameter.c_str()), pTypeLimitlessLights, sSwitch2Color, 12, 255, 0, "0;0", devname);
				m_sql.safe_query("UPDATE DeviceStatus SET Options='%q', Name='%q' , Used=1 , SwitchType=%d WHERE (ID==%llu)", ssensorname.c_str(), ssensorname.c_str(), STYPE_Dimmer, DeviceRowIdx);
				bCreated = true;
				m_mainworker.m_eventsystem.GetCurrentStates();
				}
				break;
			case 5:
				//humidity
				{
					DeviceRowIdx=m_sql.UpdateValue(HwdID, rID.c_str(), 1, pTypeHUM, sTypeTHB1, 12, 255, 0, "0", devname);
					m_sql.safe_query("UPDATE DeviceStatus SET Options='%q', Name='%q' , Used=1 WHERE (ID==%llu)", ssensorname.c_str(),ssensorname.c_str(), DeviceRowIdx);
					bCreated = true;
					m_mainworker.m_eventsystem.GetCurrentStates();
				}
				break;
			}

			m_sql.m_bAcceptNewHardware = bPrevAcceptNewHardware;

			if (bCreated)
			{
				root["status"] = "OK";
				root["title"] = "Create_MQTT_Lite_Sensor";
			}
		}
	}
}
