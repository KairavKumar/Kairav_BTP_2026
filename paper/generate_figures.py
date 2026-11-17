from pathlib import Path
import numpy as np
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
HERE=Path(__file__).resolve().parent
plt.rcParams.update({'font.family':'serif','font.size':8,'axes.labelsize':8,'legend.fontsize':7,'xtick.labelsize':7,'ytick.labelsize':7,'pdf.fonttype':42})
colors=['#2465a8','#d67120']
data={n:np.load(HERE/'figure_data'/(n+'.npz')) for n in ['stock_fvr','candidate_fvr','stock_null','candidate_null']}
time=np.arange(16000)*.005
fig,axes=plt.subplots(2,1,figsize=(3.5,2.65),sharex=True)
for ax,mode in zip(axes,['fvr','null']):
 for name,color in zip(['stock','candidate'],colors):ax.plot(time,data[name+'_'+mode]['t'],color=color,lw=.55,label='Stock' if name=='stock' else 'Proposed')
 for y in [-4.5,4.5]:ax.axhline(y,ls='--',lw=.6,color='#a82929')
 ax.set_ylabel('FVR t' if mode=='fvr' else 'Null t');ax.set_ylim(-5.4,5.4);ax.set_yticks([-4.5,0,4.5]);ax.grid(alpha=.15)
axes[0].legend(loc='upper right',ncol=2,framealpha=.9);axes[-1].set_xlabel('Time within transaction (ns)');axes[-1].set_xlim(0,80)
fig.tight_layout(pad=.45);fig.savefig(HERE/'figures/tvla.pdf');plt.close(fig)
fig,ax=plt.subplots(figsize=(3.5,1.8))
for name,color in zip(['stock','candidate'],colors):
 a=data[name+'_fvr'];power=(a['mean0']+a['mean1'])*500
 ax.plot(time[:1600],power[:1600],color=color,lw=.75,label='Stock' if name=='stock' else 'Proposed')
ax.set(xlabel='Time within transaction (ns)',ylabel='Mean power (mW)',xlim=(0,8));ax.legend(ncol=2,loc='upper right');ax.grid(alpha=.15)
fig.tight_layout(pad=.45);fig.savefig(HERE/'figures/power.pdf');plt.close(fig)
import runpy
runpy.run_path(str(HERE/'generate_flow.py'),run_name='__main__')
