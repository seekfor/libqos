
static int qos_tx_callback(void* args,int msg,char* buf,int size)
{
	switch(msg)
	{
		case LIBQOS_MESSAGE_SENDRTCP:
			app_send_rtcp(args,buf,size);
			break;
		case LIBQOS_MESSAGE_SENDRTP:
			app_send_rtp(args,buf,size);
			break;
		case LIBQOS_MESSAGE_PLIREQ:
		case LIBQOS_MESSAGE_FIRREQ:
			app_generate_idr();
			break;
		case LIBQOS_MESSAGE_REMBREQ:
			br = *((int*)buf);
			app_config_bitrate(args,br);
			break;
	}
	return 0;
}

static int qos_rx_callback(void* args,int msg,char* buf,int size)
{
	switch(msg)
	{
		case LIBQOS_MESSAGE_SENDRTCP:
			app_send_rtcp(args,buf,size);
			break;
		case LIBQOS_MESSAGE_SENDRTP:
			app_send_rtp(args,buf,size);
			break;
	}
	return 0;
}


static void* qos_tx_thread(void* args)
{
	char buf[1500];
	int size;
	void* tx = libqosCreate(LIBQOS_TX_MODE,0,0x11223344,96,0x22334455,qos_tx_callback,NULL);
	while(1)
	{
		size = app_get_rtpkt(buf);
		libqosFeedRTP(tx,buf,size);
	}
	libqosDestroy(tx);
	return args;
}


int main(int argc,char* argv[])
{
	void* rx = libqosCreate(LIBQOS_RX_MODE,0,0xaabbccdd,96,0x12345678,qos_rx_callback,NULL);
	libqos_rtpkt_t* rtp;
	while(1)
	{
		usleep(10000);
		rtp = libqosGetVideoRTP(rx);
		if(rtp)
		{
			libqos_rtpkt_t* child = rtp;
			while(child)
			{
				app_process_video_rtpkt(child);
				child = child->child;
			}
			libqosFreeRTP(qos,rtp);
		}
		rtp = libqosGetAudioRTP(rx);
		if(rtp)
		{
			libqos_rtpkt_t* child = rtp;
			while(child)
			{
				app_process_audio_rtpkt(child);
				child = child->child;
			}
			libqosFreeRTP(qos,rtp);
		}
	}
	libqosDestroy(rx);
	return 0;
}



