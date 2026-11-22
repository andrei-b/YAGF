-- Preserve the source page boundaries emitted by LatexDocumentExporter.
function RawBlock(block)
    if block.format == "latex" and block.text == "\\clearpage" then
        return pandoc.RawBlock("openxml", '<w:p><w:r><w:br w:type="page"/></w:r></w:p>')
    end
end

-- Express the figure width relative to the Word text area and keep its aspect ratio.
function Image(image)
    local fraction = (image.attributes.width or ""):match("^([%d.]+)\\linewidth$")
    if fraction then
        image.attributes.width = string.format("%.2f%%", tonumber(fraction) * 100)
        image.attributes.height = nil
        return image
    end
end
