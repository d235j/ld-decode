/* LD decoder prototype, Copyright (C) 2013 Chad Page.  License: LGPL2 */

#include <complex>
#include "ld-decoder.h"
#include "deemp.h"

inline double IRE(double in) 
{
	return (in * 140.0) - 40.0;
}

struct YIQ {
        double y, i, q;

        YIQ(double _y = 0.0, double _i = 0.0, double _q = 0.0) {
                y = _y; i = _i; q = _q;
        };
};

double clamp(double v, double low, double high)
{
        if (v < low) return low;
        else if (v > high) return high;
        else return v;
}

void aclamp(double *v, int len, double low, double high)
{
	for (int i = 0; i < len; i++) {
	        if (v[i] < low) v[i] = low;
		else if (v[i] > high) v[i] = high;
	}
}

// NTSC properties
const double freq = 8.0;	// in FSC.  Must be an even number!

const double ntsc_uline = 63.5; // usec_
const double ntsc_pline = 227.5 * freq; // pixels per line
const int    ntsc_plinei = (int)ntsc_pline;
const double dotclk = (1000000.0*(315.0/88.0)*freq); 

const double dots_usec = dotclk / 1000000.0; 

const double ntsc_blanklen = 9.2;
const double ntsc_hsynctoline = ntsc_pline * (ntsc_blanklen / 63.5);

// we want the *next* colorburst as well for computation 
const double scale_linelen = ((63.5 + ntsc_blanklen) / 63.5); 
const double scale_tgt = ntsc_pline + ntsc_hsynctoline;

// uint16_t levels
uint16_t level_m40ire = 1;
uint16_t level_0ire = 16384;
uint16_t level_7_5_ire = 16384+3071;
uint16_t level_100ire = 57344;
uint16_t level_120ire = 65535;

inline double u16_to_ire(uint16_t level)
{
	if (level == 0) return -100;
	
	return -40 + ((160.0 / 65533.0) * (double)level); 
} 

inline uint16_t ire_to_u16(double ire)
{
	if (ire <= -60) return 0;
	if (ire <= -40) return 1;

	if (ire >= 120) return 65535;	

	return (((ire + 40) / 160.0) * 65534) + 1;
} 
		
// taken from http://www.paulinternet.nl/?page=bicubic
double CubicInterpolate(uint16_t *y, double x)
{
	double p[4];
	p[0] = y[0]; p[1] = y[1]; p[2] = y[2]; p[3] = y[3];

	return p[1] + 0.5 * x*(p[2] - p[0] + x*(2.0*p[0] - 5.0*p[1] + 4.0*p[2] - p[3] + x*(3.0*(p[1] - p[2]) + p[3] - p[0])));
}

void Scale(uint16_t *buf, uint16_t *outbuf, double start, double end, double outlen)
{
	double inlen = end - start;
	double perpel = inlen / outlen; 

	for (int i = 0; i < outlen; i++) {
		double p1;
		
		p1 = start + (i * perpel);
		int index = (int)p1;
		if (index < 2) index = 2;

		outbuf[i] = CubicInterpolate(&buf[index - 1], p1 - index);
	}
}
                
double WrapAngle(double a1, double a2) {
	double v = a2 - a1;

	if (v > M_PIl) v -= (2 * M_PIl);
	else if (v <= -M_PIl) v += (2 * M_PIl);

	return v;
}

// tunables

double black_ire = 7.5;

int write_locs = -1;

double frame[505][1685];

// set up sync color heterodyne table first 
complex<double> burst_hdyne[2400];

// bhet[i] = complex(np.cos(((i / freq) * 2.0 * np.pi) + (0.0/180.0)), -(np.sin(((i / freq) * 2.0 * np.pi) + (0.0/180.0))))

Filter f_syncr(f_sync);
Filter f_synci(f_sync);

void BurstDetect(uint16_t *line, int loc, double &plevel, double &pphase) 
{
	complex<double> hdyne[100], hdyne_filt[100];
	int i, j;
	
	plevel = pphase = 0;
			
	f_syncr.clear(ire_to_u16(black_ire));
	f_synci.clear(ire_to_u16(black_ire));

	for (i = loc + 140, j = 0; i < loc + 240; i++, j++) {
		hdyne[j] = (double)line[i] * burst_hdyne[i]; 

		hdyne_filt[j] = complex<double>(f_syncr.feed(hdyne[j].real()), f_syncr.feed(hdyne[j].real()));

		if (j > 16) {
			if (abs(hdyne_filt[j]) > plevel) {
				plevel = abs(hdyne_filt[j]);
				pphase = abs(hdyne_filt[j]);
			}
		}
	}
}

int get_oline(double line)
{
	int l = (int)(line + 0.5);

	if (l < 10) return -1;
	else if (l < 262) return (l - 10) * 2;
	else if (l < 271) return -1;
	else if (l < 525) return ((l - 273) * 2) + 1;

	return -1;
}

Filter f_syncp(f_sync);
double cross = 5000;

double line = -2;

int Process(uint16_t *buf, int len)
{
	double prevf, f = 0;
	double crosspoint = -1, prev_crosspoint = -1;
	int count = 0;

	double tgt_phase = 0;

	f_syncp.clear(ire_to_u16(black_ire));

	for (int i = 0; i < len - 2048; i++) {
		prevf = f;
		f = f_syncp.feed(buf[i]); 
	
		if (f < cross) {
			if (count <= 0) {
				double d = prevf - f;
				double c = (prevf - cross) / d;

				crosspoint = (i - 1) + c; 
			}
			count++;
		} else {
			if ((count > 16) && prev_crosspoint > 0) {
				double begin = prev_crosspoint;
				double end = begin + ((crosspoint - prev_crosspoint) * scale_linelen);
				double linelen = crosspoint - prev_crosspoint; 

				uint16_t tout[4096];

				cerr << line << ' ' << linelen << ' ' << count << endl;
				if ((line >= 0) && (linelen >= (ntsc_pline * 0.9)) && (count > 90)) {
					// standard line
					double plevel1, pphase1;
					double plevel2, pphase2;
					double adjust1, adjust2;
					int oline = get_oline(line);

//					cerr << buf << ' ' <<  begin << ' ' << end << ' ' << scale_tgt << endl;

					Scale(buf, tout, begin, end, scale_tgt); 
					BurstDetect(tout, 0, plevel1, pphase1); 
					BurstDetect(tout, 1820, plevel2, pphase2); 

					if (!tgt_phase) {
						if ((pphase1 < 0) && (pphase1 > (-M_PIl * 3 / 4))) {
							tgt_phase = -M_PIl / 2.0;
						} else {
							tgt_phase = M_PIl / 2.0;
						}
					} else tgt_phase = -tgt_phase;

					adjust1 = WrapAngle(pphase1, tgt_phase);	
					begin += (adjust1 * 1.3);

					Scale(buf, tout, begin, end, scale_tgt); 
					BurstDetect(tout, 0, plevel1, pphase1); 
					BurstDetect(tout, 1820, plevel2, pphase2); 
					
					adjust2 = WrapAngle(pphase2, tgt_phase);	
					end += (adjust2 * 1.3);

					Scale(buf, tout, begin, end, scale_tgt); 

					// LD only: need to adjust output value for velocity
					double lvl_adjust = ((((end - begin) / scale_tgt) - 1) * 0.84) + 1;
					for (int x = 130; (oline > 0) && (x < (130+1685)); x++) {
						double v = tout[x];

						v = ((v / 57344.0) * 1700000) + 7600000;
						v *= lvl_adjust;
						frame[oline][x - 130] = (v - 7600000) / 1700000;
					}
		
					line++;
				} else if ((line == -1) && (linelen < 1000) && (count > 80) && (count < 160)) {
					line = 262.5;
				} else if (((line == -1) || (line > 520)) && (linelen > 1800) && (count < 80)) {
					if (line > 0) {
						write(1, frame, sizeof(frame));
					}
					tgt_phase = 0;
					line = 1;
				} else if ((line == -2) && (linelen > 1800) && (count > 80)) {
					line = -1;
				} else if ((line >= 0) && (linelen > 800) && (linelen < 1000)) {
					line += 0.5;
				} else if ((line >= 0) && (linelen > 1700)) {
					line += 1;
				} 
//				printerr(line, prev_crosspoint, crosspoint, count)
 
				if (floor(line) == 272) tgt_phase -= tgt_phase;
//				cerr << line << endl;
			}

			prev_crosspoint = crosspoint;

			count = 0;
		}
	}

	return (prev_crosspoint - 100);
}

int main(int argc, char *argv[])
{
	int rv = 0, fd = 0;
	long long dlen = -1, tproc = 0;
	//double output[2048];
	unsigned short inbuf[16384];
	unsigned char *cinbuf = (unsigned char *)inbuf;

	cerr << std::setprecision(10);
	cerr << argc << endl;
	cerr << strncmp(argv[1], "-", 1) << endl;

	if (argc >= 2 && (strncmp(argv[1], "-", 1))) {
		fd = open(argv[1], O_RDONLY);
	}

	if (argc >= 3) {
		unsigned long long offset = atoll(argv[2]);

		if (offset) lseek64(fd, offset, SEEK_SET);
	}
		
	if (argc >= 4) {
		if ((size_t)atoi(argv[3]) < dlen) {
			dlen = atoi(argv[3]); 
		}
	}

	for (int i = 0; i < 2400; i++) {
		 burst_hdyne[i] = complex<double>(cos(((i / freq) * 2.0 * M_PIl) + (0.0/180.0)), -(sin(((i / freq) * 2.0 * M_PIl) + (0.0/180.0))));
	}

	// XX: make parameter
	#if SOUND
		write_locs = 0;
	#endif

	cout << std::setprecision(8);

	rv = read(fd, inbuf, 32768);
	while ((rv > 0) && (rv < 32768)) {
		int rv2 = read(fd, &cinbuf[rv], 32768 - rv);
		if (rv2 <= 0) exit(0);
		rv += rv2;
	}

	while (rv == 32768 && ((tproc < dlen) || (dlen < 0))) {
		int plen = Process(inbuf, rv / 2);	

		tproc += plen;
                memmove(inbuf, &inbuf[plen], (16384 - plen) * 2);
                rv = read(fd, &inbuf[(16384 - plen)], plen * 2) + ((16384 - plen) * 2);
		while ((rv > 0) && (rv < 32768)) {
			int rv2 = read(fd, &cinbuf[rv], 32768 - rv);
			if (rv2 <= 0) exit(-1);
			rv += rv2;
		}	
	}

	return 0;
}

