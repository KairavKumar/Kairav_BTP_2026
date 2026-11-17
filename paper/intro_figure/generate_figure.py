from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle,Polygon,FancyArrowPatch
R=Path(__file__).resolve().parent
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':8,'pdf.fonttype':42,'mathtext.fontset':'dejavusans'})
ink='#26323a';gray='#68737b';blue='#245e86';red='#a33434'
fig=plt.figure(figsize=(7.08,2.67))
a=fig.add_axes([.015,.18,.475,.73]);a.set_xlim(0,348);a.set_ylim(0,220);a.axis('off')
b=fig.add_axes([.615,.27,.36,.58])
def line(x1,y1,x2,y2,color=gray,lw=.9):a.plot([x1,x2],[y1,y2],color=color,lw=lw)
def arrow(x1,y1,x2,y2,color=gray):a.add_patch(FancyArrowPatch((x1,y1),(x2,y2),arrowstyle='-|>',mutation_scale=7,linewidth=.8,color=color,shrinkA=0,shrinkB=0))
def flop(x,y,name,launch=False):
 a.add_patch(Rectangle((x,y-20),36,40,facecolor='#eef3f7' if launch else '#f5f6f7',edgecolor=blue if launch else gray,lw=.8))
 a.text(x+18,y+5,name,ha='center',va='center',fontsize=9,color=ink)
 a.text(x+3,y-4,'D',ha='left',va='center',fontsize=6.3,color=gray)
 a.text(x+33,y-4,'Q',ha='right',va='center',fontsize=6.3,color=gray)
 a.add_patch(Polygon([(x+13,y-20),(x+23,y-20),(x+18,y-13)],closed=True,fill=False,edgecolor=gray,lw=.7))
flop(27,114,r'$R_L$',True)
arrow(63,114,91,114)
line(91,56,91,174)
a.plot([91],[114],marker='o',markersize=2.5,color=gray)
for tag,y in [('A',174),('B',114),('C',56)]:
 arrow(91,y,113,y)
 a.add_patch(Rectangle((113,y-14),109,28,facecolor='white',edgecolor=gray,lw=.7))
 a.text(167.5,y,'Logic + wire: '+r'$d_'+tag+r'$',ha='center',va='center',fontsize=8,color=ink)
 arrow(222,y,290,y)
 a.text(257,y+13,r'$a_'+tag+r"'\!\approx a_"+tag+r'+\Delta_L$',ha='center',va='center',fontsize=7.6,color=blue)
 flop(290,y,r'$R_'+tag+r'$')
 arrow(308,y-33,308,y-20)
# A single launch-clock adjustment is shared by every fanout path.
arrow(45,71,45,94,blue)
a.text(45,57,r'$t_L\ \rightarrow\ t_L+\Delta_L$',ha='center',va='center',fontsize=8,color=blue)
a.text(45,40,'Shifted launch clock',ha='center',va='center',fontsize=7.2,color=blue)
a.text(307,211,'Capture clocks fixed',ha='center',va='center',fontsize=7.2,color=gray)
a.text(174,11,'One clock adjustment moves every launched path.',ha='center',fontsize=8,color=ink)
fig.text(.025,.96,'(a) Coupled paths from one launch register',fontsize=9,fontweight='bold',color=ink)
# Independent path timing limits, expressed in the same launch-shift variable.
b.set_xlim(-40,50);b.set_ylim(-.2,3.8)
b.axvspan(-5,10,color=blue,alpha=.09,zorder=0)
b.axvline(0,color='#a6afb5',lw=.6)
b.axvline(20,color=red,lw=.85,ls=(0,(3,2)))
rows=[('Path A',3.1,-10,40),('Path B',2.2,-30,10),('Path C',1.3,-5,25),('All paths',.3,-5,10)]
for label,y,lo,hi in rows:
 col=blue if label=='All paths' else gray
 b.plot([lo,hi],[y,y],color=col,lw=3 if label=='All paths' else 2,solid_capstyle='butt')
 b.plot([lo,lo],[y-.1,y+.1],color=col,lw=.9);b.plot([hi,hi],[y-.1,y+.1],color=col,lw=.9)
 b.text((lo+hi)/2,y+.17,rf'$[{lo:+d},\,{hi:+d}]$',ha='center',fontsize=7.3,color=col)
 if label!='All paths':b.scatter([20],[y],marker='x' if label=='Path B' else 'o',s=30 if label=='Path B' else 13,color=red if label=='Path B' else blue,zorder=5)
b.set_yticks([x[1] for x in rows],labels=[x[0] for x in rows],fontsize=8)
b.set_xticks([-30,-10,0,10,20,40]);b.tick_params(axis='x',labelsize=7.2,pad=3,length=2.5);b.tick_params(axis='y',length=0,pad=7)
for side in ['top','left','right']:b.spines[side].set_visible(False)
b.spines['bottom'].set_color(gray);b.spines['bottom'].set_linewidth(.6)
b.set_xlabel(r'Launch-clock shift $\Delta_L$ (ps)',fontsize=8,labelpad=4)
b.text(20,3.68,r'Trial $+20$ ps',ha='center',fontsize=7.5,color=red)
fig.text(.55,.96,'(b) Different path timing limits',fontsize=9,fontweight='bold',color=ink)
fig.text(.55,.09,'The +20 ps trial violates Path B\'s setup limit.',fontsize=7.7,color=red)
fig.text(.55,.025,'Shared feasible shift: intersection of all path intervals.',fontsize=7.4,color=ink)
fig.text(.025,.07,r'Fixed-delay view: $a_i=t_L+t_{CQ}+d_i$',fontsize=8.2,color=ink)
fig.savefig(R/'clock_shift_fanout.pdf');fig.savefig(R/'clock_shift_fanout.png',dpi=250);plt.close(fig)
