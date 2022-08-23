
#include "qos.h"

static int libqos_send_sr(libqos_t* qos,uint32_t pts);
static int libqos_send_rr(libqos_t* qos);
static int libqos_send_remb(libqos_t* qos);
static int libqos_send_pli(libqos_t* qos,int idx);
static int libqos_send_fir(libqos_t* qos,int idx);
static int libqos_send_nacks(libqos_t* qos,int idx,unsigned short* nack,int size);

static libqos_rtpkt_t* libqos_alloc_rtpkt(libqos_t* qos);
static int libqos_free_length(libqos_rtpkt_t* rtp);
static void libqos_free_rtpkt(libqos_t* qos,libqos_rtpkt_t* rtp);

static uint32_t READDWORD(uint8_t* buf)
{
	uint32_t ret = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
	return ret;
}

static uint16_t READWORD(uint8_t* buf)
{
	uint16_t ret = (buf[0] << 8) | buf[1];
	return ret;
}

static uint64_t READQWORD(uint8_t* buf)
{
	uint64_t msb = READDWORD(buf + 0);
	uint64_t lsb = READDWORD(buf + 4);
	uint64_t ret = (msb << 32) | lsb;
	return ret;
}

static void WRITEQWORD(uint8_t* buf,uint64_t val)
{
	buf[0] = val >> 56;
	buf[1] = val >> 48;
	buf[2] = val >> 40;
	buf[3] = val >> 32;
	buf[4] = val >> 24;
	buf[5] = val >> 16;
	buf[6] = val >> 8;
	buf[7] = val & 0xff;
}

static void WRITEDWORD(uint8_t* buf,uint32_t val)
{
	buf[0] = val >> 24;
	buf[1] = val >> 16;
	buf[2] = val >> 8;
	buf[3] = val & 0xff;
}

static void WRITEWORD(uint8_t* buf,uint32_t val)
{
	buf[0] = val >> 8;
	buf[1] = val & 0xff;
}

static uint32_t  libqos_tick()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME,&ts);
	return (ts.tv_sec * 1000ul) + (ts.tv_nsec / 1000000ul);
}

static uint64_t  libqos_tick64()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME,&ts);
	return (ts.tv_sec * 1000000ul) + (ts.tv_nsec / 1000ul);
}

static uint32_t libqos_get_remb(libqos_t* qos,uint8_t* buf,int len)
{
	unsigned int val = READDWORD(buf + 16);
	unsigned int exp = (val >> 18) & 0x3f;
	unsigned int mantissa = val & 0x3ffff;
	qos->rtcp.remb.calcbps = mantissa << exp;
	return qos->rtcp.remb.calcbps;
}

static void libqos_nack_recv(libqos_t* qos,uint8_t* buf)
{
	uint16_t nacks[1024];
	int numnacks = 0;
	uint16_t seq;
	uint16_t blp;
	int i;
	int numwords;
	int count = 0;
	libqos_rtpkt_t* rtp;
	uint8_t* tbl = buf + 12;
	uint32_t media_ssrc = READDWORD(buf + 8);
	if(media_ssrc != qos->local_video_ssrc)
	{
		return;
	}
	numwords = READWORD(buf + 2) - 2;
	while(numwords > 0)
	{
		nacks[numnacks++] = seq = READWORD(tbl + 0);
		blp = READWORD(tbl + 2);
		if(numnacks >= 1024)
		{
			break;
		}
		for(i = 0; i < 16; i ++)
		{
			if(blp & (1 << i))
			{
				nacks[numnacks++] = seq + i + 1;
				if(numnacks >= 1024)
				{
					break;
				}
			}
		}
		tbl += 4;
		numwords --;
	};
	for(i = 0; i < numnacks; i ++)
	{
		seq = nacks[i];
		rtp = qos->incoming_rtp[1][seq];
		if(!rtp)
		{
			continue;
		}
		count ++;
		qos->callback(qos->args,LIBQOS_MESSAGE_SENDRTP,rtp->buf,rtp->size);
		if(count > 32)
		{
			count = 0;
			usleep(1000);
		}
	}	
}


static int libqos_parse_rtcp(libqos_t* qos,uint8_t* buf,int size)
{
	uint8_t pt;
	uint32_t fmt;
	uint32_t v;
	int32_t len;
	uint32_t ssrc,val;
	int32_t i;
	uint8_t* t;
	libqos_sr_t* sr;
	libqos_rr_t* rr;
	libqos_ntp_t* ntp = &qos->ntp;
	while(size > 0)
	{
		v = buf[0] >> 6;
		fmt = buf[0] & 0x1f;
		pt = buf[1];
		len = READWORD(buf + 2);
		switch(pt)
		{
			case LIBQOS_RTCP_SR:/*SR*/
				ntp->rrtime = libqos_tick();
				ntp->rembtime = libqos_tick();
				ssrc = READDWORD(buf + 4);
				if(ssrc == qos->audio_ssrc)
				{
					sr = &qos->rtcp.sr[0];
				}
				else if(ssrc == qos->video_ssrc)
				{
					sr = &qos->rtcp.sr[1];
				}
				else
				{
					break;
				}
				sr->ssrc = ssrc;
				sr->ntp = READQWORD(buf + 8);
				sr->pts = READDWORD(buf + 16);
				sr->pktscount = READDWORD(buf + 20);
				sr->bytescount = READDWORD(buf + 24);
				
				ntp->ntpnow = sr->ntp;
				ntp->rtcnow = libqos_tick64();
				break;
			case LIBQOS_RTCP_RR:/*RR*/
				ssrc = READDWORD(buf + 4);
				if(!ssrc)
				{
					break;
				}
				t = buf + 8;
				for(i =	0; i < ((len - 1) / 6); i ++)
				{
					ssrc = READDWORD(t + 0);
					if(ssrc == qos->audio_ssrc)
					{
						rr = &qos->rtcp.rr[0];
					}
					else if(ssrc == qos->video_ssrc)
					{
						rr = &qos->rtcp.rr[1];
					}
					else
					{
						break;
					}
					rr->ssrc = ssrc;
					val = READDWORD(t + 4);
					rr->lost = val >> 24;
					rr->alllosts = val & 0x00ffffff;
					rr->seqmsb = READDWORD(t + 8);
					rr->jitter = READDWORD(t + 12);
					rr->lsr = READDWORD(t + 16);
					rr->dlsr = READDWORD(t + 20);
					t += 24;
				}
				break;
			case LIBQOS_RTCP_SDES:/*SDES*/
				break;
			case LIBQOS_RTCP_RTPFB:/*RTPFB*/
				switch(fmt)
				{
					case 0x01:
						libqos_nack_recv(qos,buf);
						break;
					default:
						break;
				}
				break;
			case LIBQOS_RTCP_PSFB:/*PSFB*/
				switch(fmt)
				{
					case 0x01:
						qos->callback(qos->args,LIBQOS_MESSAGE_PLIREQ,NULL,0);
						break;
					case 0x04:
						qos->callback(qos->args,LIBQOS_MESSAGE_FIRREQ,NULL,0);
						break;
					case 0x0f:
						v = libqos_get_remb(qos,buf,len);
						qos->callback(qos->args,LIBQOS_MESSAGE_REMBREQ,(uint8_t*)&v,4);
						break;
				}
				break;
		}
		len = (len + 1) * 4;
		buf += len;
		size -= len;
	}

}

static void libqos_feed_rr(libqos_t* qos,int idx,int size,int seq)
{
	int i;
	libqos_rr_t* rr = &qos->rtcp.rr[idx];
	rr->bytescount += size;
	rr->pktscount ++;
	if((seq <= qos->start[idx]) || (seq >= qos->stop[idx]) )
	{
		qos->stop[idx] = seq;
		if(rr->seq != seq)
		{
			uint32_t nextseq = (rr->seq + 1) & 0xffff;
			if(nextseq != seq)
			{
				uint32_t lost = 0;
				for(i = nextseq; i != seq; i = (i + 1) & 0xffff)
				{
					lost ++;
				}
				rr->alllosts += lost;
				rr->pktslost += lost;
				rr->pktstotal += lost;
			}
			rr->pktstotal ++;
		}
		rr->seq = seq;
	}
}

static void libqos_feed_remb(libqos_t* qos,int size)
{
	uint32_t diff;
	libqos_remb_t* remb = &qos->rtcp.remb;
	if(!remb->lastntp)
	{
		remb->lastntp = libqos_tick();
	}
	remb->nowntp = libqos_tick();
	remb->bytescount += size;
	remb->pktscount ++;
	diff = remb->nowntp - remb->lastntp;
	if(diff > 500)
	{
		uint32_t bps = remb->bytescount * 8000 / diff;
		remb->realbps = remb->realbps * 0.4f + bps * 0.6f;
		remb->lastntp = remb->nowntp;
		remb->bytescount = 0;
		remb->pktscount = 0;
	}

}

static void libqos_feed_sr(libqos_t* qos,int idx,uint32_t pts,uint8_t* buf,int size)
{
	libqos_sr_t* sr = &qos->rtcp.sr[idx];
	libqos_ntp_t* ntp = &qos->ntp;
	sr->pktscount ++;
	sr->bytescount ++;
	sr->pts = pts;
	sr->ntp = libqos_tick64();
	sr->ssrc = idx?qos->video_ssrc:qos->audio_ssrc;
	if(!ntp->srtime)
	{
		ntp->srtime = libqos_tick();
	}
	else if((libqos_tick() - ntp->srtime) > 1000)
	{
		ntp->srtime = libqos_tick();
		libqos_send_sr(qos,pts);
	}
}
		
static int libqos_feed_callback(libqos_t* qos,int idx)
{
	int start = qos->start[idx];
	int stop = qos->stop[idx];
	int i;
	libqos_remb_t* remb = &qos->rtcp.remb;
	uint32_t pts;
	libqos_rtpkt_t* rtp;
	int count = 0;
	for(i = 0; i < CFG_MAX_POOL; i++)
	{
		rtp = qos->incoming_rtp[idx][i];
		if(!rtp)
		{
			continue;
		}	
		if((libqos_tick() - rtp->utc) > 200)
		{
			qos->incoming_rtp[idx][i] = NULL;
			libqos_free_rtpkt(qos,rtp);
		}

	}
	for(i = start; i != (stop + 1); i = (i + 1) & 0xffff)
	{
		rtp = qos->incoming_rtp[idx][i];
		if(!rtp)
		{
			continue;
		}
		count ++;	
		libqos_feed_remb(qos,rtp->size);
		qos->callback(qos->args,LIBQOS_MESSAGE_SENDRTP,rtp->buf,rtp->size);
		if(count > 32)
		{
			count = 0;
			usleep(1000);
		}
	}
	qos->start[idx] = i;
	return 0;
}


int libqosFeedRTCP(void* handler,uint8_t* buf,int32_t size)
{
	int ret;
	libqos_t* qos = (libqos_t*)handler;
	if(!qos)
	{
		return -1;
	}
	libqosWaitSemaphore(qos->mutex);
	ret = libqos_parse_rtcp(qos,buf,size);
	libqosPostSemaphore(qos->mutex);
	return ret;
}

static libqos_rtpkt_t* libqos_alloc_rtpkt(libqos_t* qos)
{
	libqos_rtpkt_t* ret = qos->freed_rtp;
	if(ret)
	{
		qos->freed_rtp = ret->next;
	}
	else
	{
		ret = (libqos_rtpkt_t*)malloc(sizeof(libqos_rtpkt_t));
		if(!ret)
		{
			return NULL;
		}
	}
	memset(ret,0,sizeof(libqos_rtpkt_t));
	return ret;
}

static int libqos_free_length(libqos_rtpkt_t* rtp)
{
	int ret = 0;
	while(rtp)
	{
		ret ++;
		rtp = rtp->next;
	}
	return ret;
}


static void libqos_free_rtpkt(libqos_t* qos,libqos_rtpkt_t* rtp)
{
	libqos_rtpkt_t* next = qos->freed_rtp;

	if(libqos_free_length(next) > 64)
	{
		free(rtp);
	}
	else
	{
		rtp->child = NULL;
		rtp->next = next;
		qos->freed_rtp = rtp;
	}
}

static void libqos_destroy_rtpkt(libqos_t* qos,libqos_rtpkt_t* rtp,int destroy)
{
	libqos_rtpkt_t* child = rtp;
	while(child)
	{
		rtp = rtp->child;
		if(!destroy)
		{
			libqos_free_rtpkt(qos,child);
		}
		else
		{
			free(child);
		}
		child = rtp;
	}
}


static int libqos_store_frame(libqos_t* qos,int idx,int start)
{
	libqos_rtpkt_t* rtp = qos->incoming_rtp[idx][start];
	int i;
	int len;
	int stop = qos->stop[idx];
	unsigned int pts;
	libqos_rtpkt_t* frm = NULL;
	libqos_rtpkt_t* decode;
	if(!rtp)
	{
		return 0;
	}
	pts = rtp->pts;
	for(i = start; i != stop;i = ((i + 1) & 0xffff) )
	{
		rtp = qos->incoming_rtp[idx][i];
		if(!rtp)
		{
			continue;
		}
		else if(rtp->pts != pts)
		{
			qos->start[idx] = i;
			break;
		}
		else
		{
			qos->incoming_rtp[idx][i] = NULL;
			if(!frm)
			{
				frm = rtp;
			}
			else
			{
				decode = frm;
				while(decode->child)
				{
					decode = decode->child;
				}
				decode->child = rtp;
			}
		}
	}
	if(frm)
	{
		decode = qos->decode_rtp[idx];
		if(!decode)
		{
			qos->decode_rtp[idx] = frm;
		}
		else
		{
			len = libqos_free_length(decode);
			if(len > CFG_MAX_FRAME)
			{
				libqos_destroy_rtpkt(qos,decode,1);
				decode = qos->decode_rtp[idx] = decode->next;
			}
			while(decode->next)
			{
				decode = decode->next;
			}
			decode->next = frm;
		}
	}	
	return 0;
}

static int libqos_send_pli(libqos_t* qos,int idx)
{
	unsigned char buf[12 + CFG_EXTRA_BYTES];
	libqos_ntp_t* ntp = &qos->ntp;
	if((libqos_tick() - ntp->plitime) > 1000)
	{
		ntp->plitime = libqos_tick();
		buf[0] = 0x81;
		buf[1] = LIBQOS_RTCP_PSFB;
		WRITEWORD(buf + 2,2);
		WRITEDWORD(buf + 4,qos->local_video_ssrc);
		WRITEDWORD(buf + 8,qos->video_ssrc);
		qos->callback(qos->args,LIBQOS_MESSAGE_SENDRTCP,buf,12);
	}
	return 0;
}

static int libqos_send_fir(libqos_t* qos,int idx)
{
	unsigned char buf[16 + CFG_EXTRA_BYTES];
	libqos_ntp_t* ntp = &qos->ntp;
	if((libqos_tick() - ntp->firtime) > 1000)
	{
		ntp->firtime = libqos_tick();
		buf[0] = 0x84;
		buf[1] = LIBQOS_RTCP_PSFB;
		WRITEWORD(buf + 2,3);
		WRITEDWORD(buf + 4,qos->local_video_ssrc);
		WRITEDWORD(buf + 8,qos->video_ssrc);
		WRITEDWORD(buf + 12,qos->last_seq[idx] << 24);
		qos->callback(qos->args,LIBQOS_MESSAGE_SENDRTCP,buf,16);
	}
	return 0;
}

static void libqos_discard_frame(libqos_t* qos,int idx,unsigned int pts,int fir)
{
	int i;
	int start = qos->start[idx];
	int stop = qos->stop[idx];
	libqos_rtpkt_t* rtp;
	for(i = start; i != stop; i = ((i + 1) & 0xffff))
	{
		rtp = qos->incoming_rtp[idx][i];
		if(!rtp)
		{
			continue;
		}
		else if(rtp->pts == pts)
		{
			qos->incoming_rtp[idx][i] = NULL;
			libqos_free_rtpkt(qos,rtp);
		}
		else
		{
			qos->start[idx] = i;
			break;
		}
	}
	if((idx != 0) && (fir != -1))
	{
		if(!fir)
		{
			libqos_send_pli(qos,idx);
		}
		else
		{
			libqos_send_fir(qos,idx);
		}
	}
}

static int libqos_send_nacks(libqos_t* qos,int idx,unsigned short* nack,int size)
{
	int blp = 0;
	int words = 3;
	int i;
	unsigned char rtcp[1024];
	unsigned char* tbl;
	unsigned short seq;
	unsigned int pt = idx?qos->video_pt:qos->audio_pt;
	unsigned int ssrc = idx?qos->local_video_ssrc:qos->local_audio_ssrc;
	unsigned int media = idx?qos->video_ssrc:qos->audio_ssrc;
	rtcp[0] = 0x81;/*FMT=1,V=2*/
	rtcp[1] = LIBQOS_RTCP_RTPFB;
	rtcp[2] = 0x00;
	rtcp[3] = 0x00;
	rtcp[4] = ssrc >> 24;
	rtcp[5] = ssrc >> 16;
	rtcp[6] = ssrc >> 8;
	rtcp[7] = ssrc & 0xff;
	rtcp[8] = media >> 24;
	rtcp[9] = media >> 16;
	rtcp[10] = media >> 8;
	rtcp[11] = media & 0xff;

	tbl = rtcp + 12;
	seq = *nack++;
	size --;
	tbl[0] = seq >> 8;
	tbl[1] = seq & 0xff;
	tbl[2] = 0x00;
	tbl[3] = 0x00;
	while(size > 0)
	{
		int bt = 17;
		if(nack[0] > seq)
		{
			bt = nack[0] - seq;
		}
		if(bt > 16)
		{
			tbl[2] = blp >> 8;
			tbl[3] = blp & 0xff;

			words ++;
			tbl += 4;

			blp = 0x00;
			seq = nack[0];

			tbl[0] = seq >> 8;
			tbl[1] = seq & 0xff;
			tbl[2] = 0x00;
			tbl[3] = 0x00;

		}
		else
		{
			blp |= (1 << (bt - 1));
		}

		size--;
		nack++;

	}
	rtcp[2] = words >> 8;
	rtcp[3] = words & 0xff;
	size = (words + 1) * 4;
	qos->callback(qos->args,LIBQOS_MESSAGE_SENDRTCP,rtcp,size);
	return size;
}

static int libqos_send_remb(libqos_t* qos)
{
	int i;
	uint8_t buf[24 + CFG_EXTRA_BYTES];
	libqos_remb_t* remb = &qos->rtcp.remb;
	libqos_rr_t* rr = &qos->rtcp.rr[1];
	int exp = 0;
	int mantissa;
	int bps = remb->realbps;
	int val;
	if(rr->lost > 26) /*> 10%*/
	{
		bps *= 0.85;
	}
	else if(rr->lost < 6) /*< 2%*/
	{
		bps *= 1.05;
	}
	remb->calcbps = bps;
	for(exp = 0;exp < 32;exp ++)
	{
		if(bps < (0x3ffff << exp) )
		{
			break;
		}
	}
	if(exp > 31)
	{
		exp = 32;
	}
	mantissa = bps >> exp;


	buf[0] = 0x8f;/*FMT:15*/
	buf[1] = LIBQOS_RTCP_PSFB;
	WRITEWORD(buf + 2,5);
	WRITEDWORD(buf + 4,qos->local_video_ssrc);
	WRITEDWORD(buf + 8,0);
	memcpy(buf + 12,"REMB",4);
	val = (1 << 24) | (exp << 18) | mantissa;
	WRITEDWORD(buf + 16,val);
	WRITEDWORD(buf + 20,qos->video_ssrc);
	return qos->callback(qos->args,LIBQOS_MESSAGE_SENDRTCP,buf,24);
}

static int libqos_send_sr(libqos_t* qos,uint32_t pts)
{
	uint8_t buf[28 + CFG_EXTRA_BYTES];
	uint64_t ntp;
	libqos_sr_t* sr = &qos->rtcp.sr[1];
	int i;
	buf[0] = 0x80;
	buf[1] = LIBQOS_RTCP_SR;
	WRITEWORD(buf + 2,6);
	WRITEDWORD(buf + 4,qos->local_video_ssrc);
	ntp = libqos_tick64();
	WRITEDWORD(buf + 8,ntp >> 32);
	WRITEDWORD(buf + 12,ntp & 0xffffffff);
	WRITEDWORD(buf + 16,pts);
	WRITEDWORD(buf + 20,sr->pktscount);
	WRITEDWORD(buf + 24,sr->bytescount);
	return qos->callback(qos->args,LIBQOS_MESSAGE_SENDRTCP,buf,28);
}

static int libqos_send_rr(libqos_t* qos)
{
	int i;
	uint8_t buf[56 + CFG_EXTRA_BYTES];
	libqos_rr_t* rr = &qos->rtcp.rr[0];
	libqos_ntp_t* ntp = &qos->ntp;
	uint8_t* item;
	uint32_t val;
	uint32_t lost = 0;
	buf[0] = 0x82;
	buf[1] = LIBQOS_RTCP_RR;
	WRITEWORD(buf + 2,13);
	WRITEDWORD(buf + 4,qos->video_ssrc);
	item = buf + 8;
	for(i = 0; i < 2; i ++,rr++)
	{
		if(!rr->ssrc)
		{
			if(i)
			{
				rr->ssrc = qos->video_ssrc;
			}
			else
			{
				rr->ssrc = qos->audio_ssrc;
			}
		}
		WRITEDWORD(item + 0,rr->ssrc);
		if(rr->pktstotal)
		{
			rr->lost = rr->pktslost * 256 / rr->pktstotal;
		}
		else
		{
			rr->lost = 0;
		}
		val = (rr->lost << 24) | rr->alllosts;
		WRITEDWORD(item + 4,val);
		WRITEDWORD(item + 8,rr->seqmsb);
		WRITEDWORD(item + 12,0);
		val = ntp->ntpnow & 0xffffffff;
		WRITEDWORD(item + 16,val);
		val = (libqos_tick64() - ntp->rtcnow) / 1000.0f / 1000.0f / 1000.0f * 65536.0f;
		WRITEDWORD(item + 20,val);
		item += 24;

		rr->pktslost = 0;
		rr->pktstotal = 0;
	}
	return qos->callback(qos->args,LIBQOS_MESSAGE_SENDRTCP,buf,56);
}


static int libqos_rtcp_other_send(libqos_t* qos,int idx)
{
	libqos_ntp_t* ntp = &qos->ntp;

	if(qos->mode == LIBQOS_TX_MODE)
	{
		return 0;
	}
	if(!ntp->ntpnow)
	{
		return -1;
	}
	if(ntp->rrtime && ((libqos_tick() - ntp->rrtime) > 100))
	{
		ntp->rrtime = 0;
		libqos_send_rr(qos);
	}
	if(ntp->rembtime && ((libqos_tick() - ntp->rembtime) > 100))
	{
		ntp->rembtime = 0;
		libqos_send_remb(qos);
	}

	return 0;

}

static int libqos_rtcp_nack_send(libqos_t* qos,int idx)
{
	libqos_rtpkt_t* rtp = NULL;
	int i;
	unsigned short nacks[256];
	int numnacks = 0;
	int start = qos->start[idx];
	int last = qos->stop[idx];	
	unsigned int pts = 0;
	int ok = 0;
	int send = 0;
	uint32_t utc;

	if(idx == 0)
	{
		for(i = start;i != last; i = ((i + 1) & 0xffff))
		{
			rtp = qos->incoming_rtp[idx][i];
			if(!rtp)
			{
				continue;
			}
			else
			{
				start = i;
				break;
			}
		}
		libqos_store_frame(qos,idx,start);
		return 0;
	}

	rtp = qos->incoming_rtp[idx][start];
	if(!rtp)
	{
		for(i = start;i != last; i = ((i + 1) & 0xffff))
		{
			rtp = qos->incoming_rtp[idx][i];
			if(!rtp)
			{
				continue;
			}
			qos->start[idx] = start = i;
			break;
		}
		return 0;
	}
	else
	{
		utc = rtp->utc;
		pts = rtp->pts;
		if((libqos_tick() - utc) > 200)
		{
			libqos_discard_frame(qos,idx,pts,-1);
			for(i = start;i != last; i = ((i + 1) & 0xffff))
			{
				rtp = qos->incoming_rtp[idx][i];
				if(!rtp)
				{
					continue;
				}
				qos->start[idx] = start = i;
				break;
			}
			return 0;
		}
		else
		{
			for(i = start;i != last; i = ((i + 1) & 0xffff))
			{
				rtp = qos->incoming_rtp[idx][i];
				if(!rtp)
				{
					break;				
				}
				else if(rtp->pts != pts)
				{
					ok = 1;
					break;
				}
			}
			if(ok)
			{
				libqos_store_frame(qos,idx,start);
				start = qos->start[idx];
			}
		}
	}

	for(i = start; i != last; i = ((i + 1) & 0xffff))
	{
		rtp = qos->incoming_rtp[idx][i];
		if(!rtp)
		{
			nacks[numnacks++] = i;
			if(numnacks >= 128)
			{
				break;
			}
		}
		else
		{
			int diff = libqos_tick() - rtp->utc;
			if(rtp->nacks > CFG_NACK_TIMEOUT)
			{
				libqos_discard_frame(qos,idx,rtp->pts,0);
				return 0;
			}
			else if(diff > CFG_NACK_PERIOD)
			{
				rtp->nacks ++;
				rtp->utc = libqos_tick();
				send = 1;
			}
		}
	}
	if(numnacks && send)
	{
		if(numnacks < 128)
		{
			libqos_send_nacks(qos,idx,nacks,numnacks);
		}
		else
		{
			libqos_discard_frame(qos,idx,pts,1);
		}
	}

	return 0;
}
	
int libqosFeedRTP(void* handler,unsigned char* buf,int size)
{
	int i;
	int mark;
	int idx;
	unsigned int pt,pts,ssrc;
	int seq;
	libqos_t* qos = (libqos_t*)handler;
	libqos_rtpkt_t* rtp,*frm;
	libqos_rr_t* rr;
	libqos_ntp_t* ntp;

	if(!qos || !buf || (size <= 0) || (size > 1500))
	{
		return -1;
	}
	
	libqosWaitSemaphore(qos->mutex);

	pt = buf[1] & 0x7f;
	mark = (buf[1] & 0x80)?1:0;
	seq = READWORD(buf + 2);
	pts = READDWORD(buf + 4);
	ssrc = READDWORD(buf + 8);

	if(qos->mode == LIBQOS_RX_MODE)
	{
		if(ssrc == qos->audio_ssrc)
		{
			idx = 0;
		}
		else if(ssrc == qos->video_ssrc)
		{
			idx = 1;
		}
		else
		{
			libqosPostSemaphore(qos->mutex);
			return -1;
		}
	}
	else
	{
		if(ssrc == qos->local_audio_ssrc)
		{
			idx = 0;
		}
		else if(ssrc == qos->local_video_ssrc)
		{
			idx = 1;
		}
		else
		{
			libqosPostSemaphore(qos->mutex);
			return -1;
		}
	}
	
	if(qos->incoming_rtp[idx][seq])
	{
		libqosPostSemaphore(qos->mutex);
		return -1;
	}
	
	rtp = libqos_alloc_rtpkt(qos);
	if(rtp)
	{
		rtp->utc = libqos_tick();
		rtp->pt = pt;
		rtp->seq = seq;
		rtp->pts = pts;
		rtp->ssrc = ssrc;
		rtp->mark = mark;
		memcpy(rtp->buf,buf,size);
		rtp->size = size;
		qos->incoming_rtp[idx][seq] = rtp;
	}
	qos->last_seq[idx] = seq;	

	if(qos->mode == LIBQOS_TX_MODE)
	{
		qos->stop[idx] = seq;
		libqos_feed_sr(qos,idx,pts,buf,size);
		if(mark)
		{
			libqos_feed_callback(qos,idx);
		}
	}
	else
	{
		libqos_feed_remb(qos,size);
		libqos_feed_rr(qos,idx,size,seq);
		libqos_rtcp_nack_send(qos,idx);
		libqos_rtcp_other_send(qos,idx);
	}
	libqosPostSemaphore(qos->mutex);
	return 0;
}

void* libqosCreate(int mode,int audio_pt,int audio_ssrc,int video_pt,int video_ssrc,libqos_callback callback,void* args)
{
	libqos_t* qos;
	if(!callback)
	{
		return NULL;
	}
	qos = (libqos_t*)malloc(sizeof(libqos_t));
	if(!qos)
	{
		return qos;
	}
	memset(qos,0,sizeof(libqos_t));
	qos->mode = mode;
	qos->audio_pt = audio_pt;
	qos->audio_ssrc = audio_ssrc;
	qos->video_pt = video_pt;
	qos->video_ssrc = video_ssrc;
	if(mode == LIBQOS_TX_MODE)
	{
		qos->local_video_ssrc = video_ssrc;
		qos->local_video_pt = video_pt;
		qos->local_audio_ssrc = audio_ssrc;
		qos->local_audio_pt = audio_pt;
	}
	qos->callback = callback;
	qos->args = args;
	qos->start[0] = 0;
	qos->stop[1] = 0;
	qos->local_video_ssrc = 1;
	qos->local_audio_ssrc = 2;
	qos->mutex = libqosCreateSemaphore("MTX-LIBQOS",1);
	return qos;
}

int libqosSetLocalOptions(void* handler,int video_ssrc,int audio_ssrc,int options)
{
	libqos_t* qos = (libqos_t*)handler;
	if(!qos)
	{
		return -1;
	}
	libqosWaitSemaphore(qos->mutex);
	qos->local_video_ssrc = video_ssrc;
	qos->local_audio_ssrc = audio_ssrc;
	qos->local_options = options;
	libqosPostSemaphore(qos->mutex);
	return 0;
}

	
libqos_rtpkt_t* libqosGetVideoRTP(void* handler)
{
	libqos_t* qos = (libqos_t*)handler;
	libqos_rtpkt_t* rtp;
	if(!qos)
	{
		return NULL;
	}
	libqosWaitSemaphore(qos->mutex);
	rtp = qos->decode_rtp[1];
	if(rtp)
	{
		qos->decode_rtp[1] = rtp->next;
	}
	libqosPostSemaphore(qos->mutex);
	return rtp;
}
	
libqos_rtpkt_t* libqosGetAudioRTP(void* handler)
{
	libqos_t* qos = (libqos_t*)handler;
	libqos_rtpkt_t* rtp;
	if(!qos)
	{
		return NULL;
	}
	libqosWaitSemaphore(qos->mutex);
	rtp = qos->decode_rtp[0];
	if(rtp)
	{
		qos->decode_rtp[0] = rtp->next;
	}
	libqosPostSemaphore(qos->mutex);
	return rtp;
}

int libqosFreeRTP(void* handler,libqos_rtpkt_t* rtp)
{
	libqos_t* qos = (libqos_t*)handler;
	if(!qos || !rtp)
	{
		return -1;
	}
	libqosWaitSemaphore(qos->mutex);
	libqos_destroy_rtpkt(qos,rtp,0);
	libqosPostSemaphore(qos->mutex);
	return 0;
}

int libqosDestroy(void* handler)
{
	int i,j;
	libqos_rtpkt_t* rtp,*next,*child;
	libqos_t* qos = (libqos_t*)handler;
	if(!qos)
	{
		return -1;
	}
	libqosWaitSemaphore(qos->mutex);
	rtp = qos->freed_rtp;
	while(rtp)
	{
		next = rtp->next;
		free(rtp);
		rtp = next;
	}
	for(i = 0; i < 2; i ++)
	{
		rtp = qos->decode_rtp[i];
		while(rtp)
		{
			next = rtp->next;
			libqos_destroy_rtpkt(qos,rtp,1);
			rtp = next;
		}
	}
	for(j = 0; j < 2; j ++)
	{
		for(i = 0; i < CFG_MAX_POOL; i ++)
		{
			rtp = qos->incoming_rtp[j][i];
			if(!rtp)
			{
				continue;
			}
			libqos_destroy_rtpkt(qos,rtp,1);
		}
	}
	libqosPostSemaphore(qos->mutex);
	libqosDestroySemaphore(qos->mutex);
	free(qos);
	return 0;
}

void* libqosCreateSemaphore(char* name,int level)
{
        sem_t* ret;
        ret = (sem_t*)malloc(sizeof(sem_t) + strlen(name));
        if(ret)
        {
                sem_init(ret,0,level);
        }
        return ret;
}

int libqosWaitSemaphore(void* sem)
{
        if(!sem)
        {
                return -1;
        }
        return sem_wait((sem_t*)sem);
}

int libqosPostSemaphore(void* sem)
{
        if(!sem)
        {
                return -1;
        }
        return sem_post((sem_t*)sem);
}

int libqosDestroySemaphore(void* sem)
{
        if(sem)
        {
                sem_destroy((sem_t*)sem);
                free(sem);
        }
        return 0;
}

int libqosGetTickCount()
{
	return libqos_tick();
}

int libqosCreateThread(int prior,void* (*route)(void* args),void* args,int stack)
{
        pthread_t ret;
        pthread_attr_t attr;
        struct sched_param thread_param;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr,stack?stack:256000);
	pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
	if(0 == pthread_create(&ret,&attr,route,args))
	{
		pthread_attr_destroy(&attr);
		return ret;
	}
	pthread_attr_destroy(&attr);
        return -1;
}

int libqosJoinThread(int tid)
{
	void* ret;
	pthread_join(tid,&ret);
	return 0;
}

