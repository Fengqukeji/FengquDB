"""Draw source-based engineering schematics for the bilingual README."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
    font = Path('C:/Windows/Fonts/msyh.ttc')
    if font.exists():
        font_manager.fontManager.addfont(str(font))
        family = font_manager.FontProperties(fname=str(font)).get_name()
    else:
        family = 'DejaVu Sans'
    plt.rcParams.update({'font.family': family, 'svg.fonttype': 'none'})
    output = ROOT/'docs/assets/design'
    output.mkdir(parents=True, exist_ok=True)

    def canvas(title, subtitle, height=9):
        fig, ax = plt.subplots(figsize=(10, height), dpi=120)
        fig.subplots_adjust(left=.025, right=.975, bottom=.025, top=.86)
        ax.set(xlim=(0,1), ylim=(0,1)); ax.axis('off')
        fig.text(.05,.96,title,fontsize=22,weight='bold',color='#152a2d',va='top')
        fig.text(.05,.91,subtitle,fontsize=12,color='#506264',va='top')
        return fig, ax

    def box(ax, x, y, text, width=.65, height=.095, fill='#e9f1eb', size=15):
        ax.add_patch(FancyBboxPatch((x-width/2,y-height/2),width,height,
                                   boxstyle='round,pad=.008,rounding_size=.012',
                                   linewidth=1.2,edgecolor='#8d9c94',facecolor=fill,zorder=2))
        ax.text(x,y,text,ha='center',va='center',fontsize=size,color='#152a2d',zorder=3)

    def arrow(ax, start, end, label=None, dashed=False):
        ax.add_patch(FancyArrowPatch(start,end,arrowstyle='-|>',mutation_scale=16,
                                    linewidth=1.5,color='#126247',linestyle='--' if dashed else '-',zorder=1))
        if label:
            ax.text((start[0]+end[0])/2+.014,(start[1]+end[1])/2,label,
                    fontsize=11,color='#506264',va='center',ha='left',backgroundcolor='white')

    def save(fig, name, lang):
        fig.savefig(output/(name+'-'+lang+'.png'),dpi=120,
                    metadata={'Description':'Source-based engineering schematic; no performance attribution or simulated measurements.'})
        fig.savefig(output/(name+'-'+lang+'.svg'))
        plt.close(fig)

    for lang in ('en','zh'):
        zh = lang=='zh'
        fig,ax=canvas('01  热读：少走一层，复用已有数据' if zh else '01  Hot reads: stay in-process and reuse bytes',
                      '共享接口借用受生命周期保护的字节；普通复制接口仍然复制。' if zh else 'Shared APIs borrow lifetime-protected bytes; ordinary copying APIs still copy.')
        labels=['业务调用','进程内 C / C++ 接口','版本、过期时间与引用校验','复用缓存值','重新定位索引与数据','共享只读句柄 / 只解析所需字段','业务结果'] if zh else ['Application call','In-process C / C++ API','Check version, expiry and reference','Reuse cached value','Resolve index and value','Shared handle / parse required fields','Business result']
        box(ax,.5,.93,labels[0]);box(ax,.5,.76,labels[1]);box(ax,.5,.59,labels[2])
        arrow(ax,(.5,.88),(.5,.81));arrow(ax,(.5,.71),(.5,.64))
        box(ax,.25,.4,labels[3],width=.4);box(ax,.75,.4,labels[4],width=.4,size=13)
        arrow(ax,(.36,.54),(.25,.45),'有效' if zh else 'valid');arrow(ax,(.64,.54),(.75,.45),'失效' if zh else 'refresh')
        box(ax,.5,.22,labels[5]);box(ax,.5,.055,labels[6])
        arrow(ax,(.25,.35),(.4,.27));arrow(ax,(.75,.35),(.6,.27));arrow(ax,(.5,.17),(.5,.105))
        save(fig,'hot-read',lang)

        fig,ax=canvas('02  二进制减法：裁剪一次，复用缓冲' if zh else '02  Binary subtraction: crop and reuse a buffer',
                      '共享旧值保持不可变；必要时先复制一次，隔离快照和读者。' if zh else 'Published old bytes remain immutable; make an isolation copy when required.')
        ax.text(.07,.94,'旧值 / 已发布' if zh else 'Old / published',fontsize=15,color='#152a2d',weight='bold')
        for x,w,t,c in [(.22,.29,'保留前缀' if zh else 'Keep prefix','#e9f1eb'),(.51,.25,'删除区间' if zh else 'Remove span','#f5e5df'),(.79,.29,'保留尾部' if zh else 'Keep tail','#e9f1eb')]:
            box(ax,x,.83,t,width=w,height=.11,fill=c)
        box(ax,.5,.61,'必要时隔离复制一次到私有连续缓冲区' if zh else 'One isolation copy into a private contiguous buffer',height=.11,size=13)
        arrow(ax,(.5,.77),(.5,.67))
        box(ax,.5,.41,'裁掉指定区间；仅在长度变化时搬移尾部' if zh else 'Crop the span; move the tail only when length changes',height=.11,size=13)
        arrow(ax,(.5,.55),(.5,.47))
        ax.text(.07,.26,'新值 / 私有' if zh else 'New / private',fontsize=15,color='#152a2d',weight='bold')
        for x,w,t in [(.25,.32,'保留前缀' if zh else 'Keep prefix'),(.56,.25,'新字节' if zh else 'New bytes'),(.81,.22,'保留尾部' if zh else 'Keep tail')]:box(ax,x,.18,t,width=w,height=.1,size=13)
        arrow(ax,(.5,.35),(.5,.24))
        ax.text(.5,.045,'独占且容量合适时，可将同一缓冲转入读缓存。' if zh else 'Transfer the same buffer into cache when ownership and capacity allow.',
                fontsize=12,color='#506264',ha='center',va='center')
        save(fig,'binary-subtraction',lang)

        fig,ax=canvas('03  写入确认：性能与可靠路径' if zh else '03  Writes: performance and reliable confirmation',
                      '默认 50 ms / 1 MiB；空闲且无待写内容时休眠，不空刷盘。' if zh else 'Default 50 ms / 1 MiB; idle databases with no pending bytes sleep without flushing.',height=10)
        box(ax,.5,.94,'并发写请求 → 现有写线程执行与合组' if zh else 'Concurrent requests → writer execution and grouping',width=.84,size=14)
        box(ax,.5,.77,'本笔确认方式' if zh else 'Confirmation for this write',width=.5)
        arrow(ax,(.5,.89),(.5,.82))
        box(ax,.24,.59,'性能：发布内存新视图' if zh else 'Performance: publish\nin-memory view',width=.39,height=.12,size=14)
        box(ax,.76,.59,'可靠 / 强制可靠' if zh else 'Reliable / force reliable',width=.39,height=.12,size=14)
        arrow(ax,(.36,.72),(.24,.66));arrow(ax,(.64,.72),(.76,.66))
        box(ax,.24,.4,'返回成功，新读可见' if zh else 'Return success;\nnew reads see the value',width=.39,height=.12,size=14)
        arrow(ax,(.24,.53),(.24,.47))
        box(ax,.24,.2,'周期 / 阈值 / 显式同步' if zh else 'Period / threshold /\nexplicit sync',width=.39,height=.12,size=13)
        arrow(ax,(.24,.34),(.24,.27),dashed=True)
        box(ax,.76,.33,'物理写入 + 持久化屏障' if zh else 'Physical writes +\npersistence barrier',width=.39,height=.12,size=14)
        arrow(ax,(.76,.53),(.76,.40));arrow(ax,(.44,.2),(.57,.3))
        box(ax,.76,.08,'可靠路径：发布持久视图后确认' if zh else 'Reliable path: publish\ndurable view, then confirm',width=.43,height=.12,size=13)
        arrow(ax,(.76,.27),(.76,.15))
        ax.text(.04,.035,'性能模式未落盘的数据仍有崩溃丢失风险。' if zh else 'Pending performance-mode bytes\ncan still be lost on a crash.',
                fontsize=10,color='#506264',ha='left',va='center')
        save(fig,'write-confirmation',lang)
    print('Exported six bilingual PNG diagrams and editable SVG counterparts.')


if __name__=='__main__':
    main()
