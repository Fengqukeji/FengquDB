# 赞助收款码展示核查

核查日期：2026-11-15。已完成私有 GitHub 草稿与本地预览，尚未创建公开 GitHub 仓库或发布收款页面。两张图片作为项目资产随用户授权的源码提交保存。

## GitHub 规则

[GitHub 可接受使用政策第 10 节](https://docs.github.com/en/site-policy/acceptable-use-policies/github-acceptable-use-policies#10-advertising-on-github)允许 README 中与项目相关的静态图片、链接和宣传文字；内容主体应仍然是项目，不做垃圾推广或误导宣传。

[GitHub 附加产品条款 Pages 节](https://docs.github.com/en/site-policy/github-terms/github-terms-for-additional-products-and-features#13-pages)也明确允许部分变现方式，例如捐助按钮和众筹链接。这不等于允许把 Pages 当作电商或支付服务运营。

因此，为自己的开源项目放置自愿赞助收款码，在已查 GitHub 平台规则下有依据。普通微信/支付宝收款入口不是 GitHub Sponsors 计划；未申请或开通该计划，不冒充官方赞助支付。

现成例子：[TruthHun/BookChat 的“支持作者”](https://github.com/TruthHun/BookChat#支持作者)。其 README 直接展示支付宝和微信二维码，原文可在 [README](https://raw.githubusercontent.com/TruthHun/BookChat/master/README.md) 核对。其他项目这样做是实践例子，不代表平台对任意收款活动单独背书。

## 提供的两张图片

- `D:/linshi/zfb.jpg`：原样复制为 `docs/assets/sponsors/alipay.jpg`。
- `D:/linshi/wx.jpg`：原样复制为 `docs/assets/sponsors/wechat.jpg`。
- 离线解码确认支付宝码使用 `https://qr.alipay.com`，微信码使用 `wxp` 协议；没有访问收款链接，没有进行支付或账户操作。
- 图像呈现为收款码，未发现付款条形码或登录验证码。离线识别不能证明账号归属、实际到账或平台风控结果；支付者仍需在支付 App 内确认收款人。

## 安全范围

收款码用于让别人向对应账户付款，单凭它通常不能从收款人的账户扣钱。微信图本身包含头像、昵称及部分姓名；公开后，图片及这些信息可以被复制、长期缓存或被其他人冒用。付款页还可能显示支付平台按其规则展示的收款人信息。

首页只把两张原图作为项目自愿支持入口，提示付款前核对收款人，不创建自动退款、收款监听或第三方代收流程。不要把付款码、登录二维码、验证码或支付密钥放入仓库。若收到异常付款或退款请求，应从官方支付 App 的原交易和客服渠道核实。

正式公开前应接受上述身份信息展示范围。若不希望公开昵称或部分姓名，可由账户所有者在支付平台生成适合公开赞助的收款/赞赏码后替换；本轮不修改二维码像素或伪造隐私处理。公开 Git 历史中的图片不能保证通过后续删除彻底撤回。

GitHub 层面允许展示，不替代微信/支付宝对具体收款用途的条款，也不替代所在地税务及其他适用法律要求。文案使用“自愿支持项目维护”，不宣称公益募捐资格、免税、投资回报或固定商业权益。
