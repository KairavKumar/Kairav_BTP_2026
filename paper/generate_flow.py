from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
R=Path(__file__).resolve().parent
plt.rcParams.update({'font.family':'DejaVu Sans','pdf.fonttype':42,'font.size':7.2})
fig=plt.figure(figsize=(3.48,3.66));ax=fig.add_axes([0,0,1,1]);ax.set_xlim(0,348);ax.set_ylim(0,366);ax.axis('off')
ink='#24303a';line='#67727b';fill='#f5f6f7';accent='#294f6b'
def box(x,y,w,h,title,subtitle=None,number=None,emphasis=False,small=False):
 ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle='round,pad=0,rounding_size=3',facecolor='#eef3f6' if emphasis else fill,edgecolor=accent if emphasis else line,linewidth=.65))
 left=x+(22 if number else 7);cx=(left+x+w-7)/2
 if number:
  ax.text(x+11,y+h/2,str(number),ha='center',va='center',fontsize=7.1,fontweight='bold',color=accent)
  ax.plot([x+22,x+22],[y+6,y+h-6],color='#c5cdd3',lw=.5)
 if subtitle:
  ax.text(cx,y+h*.66,title,ha='center',va='center',fontsize=7.1 if small else 7.7,fontweight='bold',color=ink)
  ax.text(cx,y+h*.29,subtitle,ha='center',va='center',fontsize=6.3 if small else 6.65,color=ink)
 else:ax.text(x+w/2,y+h/2,title,ha='center',va='center',fontsize=6.8,color=ink,linespacing=1.25)
def arrow(x1,y1,x2,y2):
 ax.add_patch(FancyArrowPatch((x1,y1),(x2,y2),arrowstyle='-|>',mutation_scale=7,linewidth=.75,color=line,shrinkA=0,shrinkB=0))
box(17,328,152,30,'Placed masked netlist\nCell library and RC models')
box(179,328,152,30,'Secret/share provenance\nStructural gate priorities')
# Two distinct inputs merge above the first processing stage.
ax.plot([93,93,255,255],[328,319,319,328],lw=.7,color=line);arrow(174,319,174,310)
stages=[(274,'Timing and security inputs','Source-preserving STA and marked-gate weights'),(224,'LP clock scheduling','Requested register arrivals with timing constraints'),(174,'Compatible sink grouping','Split stock leaf groups by requested clock times'),(124,'Cost-aware tree construction','Bounded DP, shared branches and mixed buffers'),(74,'Physical timing validation','Clock-cell placement, achieved STA and repair')]
for i,(y,title,sub) in enumerate(stages,1):
 box(25,y,298,36,title,sub,i,emphasis=i==4)
 if i<5:arrow(174,y,174,y-14)
# Measurement branches share the same saved circuit.
ax.plot([174,174],[74,65],color=line,lw=.75);ax.plot([93,255],[65,65],color=line,lw=.75)
arrow(93,65,93,54);arrow(255,65,255,54)
box(17,15,152,39,'Alignment and cost','Weighted spread and cell area',small=True)
box(179,15,152,39,'VCS / PrimeTime PX','Power traces to Python TVLA',small=True)
ax.text(174,3,'Data logic and register placement remain fixed',ha='center',va='bottom',fontsize=6.4,color=line)
fig.savefig(R/'figures/flow.pdf');fig.savefig(R/'figures/flow.png',dpi=240);plt.close(fig)
